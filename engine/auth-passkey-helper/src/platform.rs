//! Request-scoped Ferrari Bluetooth preparation. Call only after request validation.
//! Normal/error cancellation must await the bounded `restore` attempt; a D-Bus
//! failure cannot guarantee power restoration. Drop only attempts wake unlock
//! synchronously. The finite kernel timeout also bounds a SIGKILL wake leak.

use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::Path;
use std::process::Stdio;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;
use tokio::process::Command;
use tokio::time::{sleep, timeout};

const ACQUIRE_BUDGET: Duration = Duration::from_secs(12);
const RESTORE_BUDGET: Duration = Duration::from_secs(3);
const WAKE_NANOSECONDS: u64 = 130_000_000_000;
const ADAPTERS: &str = "/sys/class/bluetooth";
const DRIVER: &str = "/sys/bus/serial/drivers/btnxpuart";

pub struct PlatformGuard(Guard<System>);

impl PlatformGuard {
    pub async fn acquire(cancelled: impl std::future::Future<Output = ()>) -> Result<Self, ()> {
        // Reuse the caller's already-registered signal receivers, including any
        // queued cancellation. Await acquisition so its error path can restore.
        Guard::acquire_until(System::new(), ACQUIRE_BUDGET, RESTORE_BUDGET, cancelled)
            .await
            .map(Self)
    }

    pub async fn restore(&mut self) {
        self.0.restore().await;
    }
}

// A small injectable hardware boundary lets tests exercise the same restoration
// state machine without touching sysfs, D-Bus, services or kernel modules.
trait Platform {
    fn wake(&mut self) -> Result<(), ()>;
    fn unlock(&mut self);
    fn created_adapter(&self) -> bool;
    async fn prepare(&mut self) -> Result<bool, ()>;
    async fn set_powered(&mut self, powered: bool) -> Result<(), ()>;
}

struct Guard<P: Platform> {
    platform: P,
    restore_power: Option<bool>,
    restore_budget: Duration,
    restored: bool,
}

impl<P: Platform> Guard<P> {
    async fn acquire(platform: P, acquire_budget: Duration, restore_budget: Duration) -> Result<Self, ()> {
        Self::acquire_until(platform, acquire_budget, restore_budget, std::future::pending()).await
    }

    async fn acquire_until(platform: P, acquire_budget: Duration, restore_budget: Duration,
        cancelled: impl std::future::Future<Output = ()>) -> Result<Self, ()> {
        let mut guard = Self { platform, restore_power: None, restore_budget, restored: false };
        let result = tokio::select! {
            biased;
            _ = cancelled => Err(()),
            result = timeout(acquire_budget, async {
            guard.platform.wake()?;
            let powered = guard.platform.prepare().await?;
            // A newly introduced adapter did not have an enabled prior state.
            if guard.platform.created_adapter() || !powered {
                guard.restore_power = Some(false);
            }
            if !powered {
                // Record the restoration before the D-Bus write: its acknowledgement
                // may time out even though the adapter changed power state.
                guard.platform.set_powered(true).await?;
            }
            Ok(())
            }) => result.map_err(|_| ()).and_then(|value| value),
        };
        if result.is_ok() {
            Ok(guard)
        } else {
            guard.restore().await;
            Err(())
        }
    }

    async fn restore(&mut self) {
        if self.restored { self.platform.unlock(); return; }
        self.restored = true;
        let target = self.restore_power.or_else(|| self.platform.created_adapter().then_some(false));
        if let Some(powered) = target {
            if matches!(timeout(self.restore_budget, self.platform.set_powered(powered)).await, Ok(Ok(()))) {
                self.restore_power = None;
            }
        }
        self.platform.unlock();
    }
}

impl<P: Platform> Drop for Guard<P> {
    fn drop(&mut self) {
        self.platform.unlock();
    }
}

struct System {
    wake_name: String,
    wake_held: bool,
    created: bool,
    session: Option<bluer::Session>,
    adapter: Option<(String, bluer::Adapter)>,
    lease: Option<fs::File>,
}

impl System {
    fn new() -> Self {
        static SEQUENCE: AtomicU64 = AtomicU64::new(0);
        Self {
            wake_name: format!("rmweb-passkey-{}-{}", std::process::id(), SEQUENCE.fetch_add(1, Ordering::Relaxed)),
            wake_held: false,
            created: false,
            session: None,
            adapter: None,
            lease: None,
        }
    }

    async fn adapter(&mut self) -> Result<bluer::Adapter, ()> {
        let name = sole_builtin_adapter(Path::new(ADAPTERS), Path::new(DRIVER))?;
        if let Some((previous, adapter)) = &self.adapter {
            // Recheck kernel ownership before each write; never switch to a
            // different adapter because BlueZ changed its default selection.
            if previous != &name { return Err(()); }
            return Ok(adapter.clone());
        }
        if self.session.is_none() {
            self.session = Some(bluer::Session::new().await.map_err(|_| ())?);
        }
        let adapter = self.session.as_ref().ok_or(())?.adapter(&name).map_err(|_| ())?;
        self.adapter = Some((name, adapter.clone()));
        Ok(adapter)
    }
}

impl Platform for System {
    fn wake(&mut self) -> Result<(), ()> {
        // Keep one process responsible for prior-power restoration. Never unlink
        // this file: replacing its inode would allow overlapping leases.
        self.lease = Some(acquire_lease(Path::new("/run/rmweb-passkey.lock"), 0)?);
        let mut model = Vec::new();
        let file = OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open("/sys/firmware/devicetree/base/model").map_err(|_| ())?;
        if !file.metadata().map_err(|_| ())?.is_file() { return Err(()); }
        file.take(129).read_to_end(&mut model).map_err(|_| ())?;
        if !ferrari_model(&model) { return Err(()); }
        self.wake_held = true;
        write_sysfs("/sys/power/wake_lock", format!("{} {}", self.wake_name, WAKE_NANOSECONDS).as_bytes())
    }

    fn unlock(&mut self) {
        if self.wake_held {
            if write_sysfs("/sys/power/wake_unlock", self.wake_name.as_bytes()).is_ok() {
                self.wake_held = false;
            }
        }
        self.lease.take();
    }

    fn created_adapter(&self) -> bool { self.created }

    async fn prepare(&mut self) -> Result<bool, ()> {
        let (builtin, count) = builtin_adapter(Path::new(ADAPTERS), Path::new(DRIVER))?;
        // The transport library selects its own adapter. With no public pinning
        // API, require the built-in to be the only available adapter.
        if builtin.is_some() && count != 1 { return Err(()); }
        if builtin.is_none() {
            // The presence of an unrelated adapter is not permission to alter it.
            if count != 0 { return Err(()); }
            self.created = true;
            command("/sbin/modprobe", &["btnxpuart"]).await?;
            command("/bin/systemctl", &["start", "bluetooth.service"]).await?;
        }
        // Existing built-in adapters must already expose their prior BlueZ power
        // state. Do not restart a service or reload a pre-existing driver.
        loop {
            if let Ok(adapter) = self.adapter().await {
                if let Ok(powered) = adapter.is_powered().await {
                    return Ok(powered);
                }
            }
            sleep(Duration::from_millis(200)).await;
        }
    }

    async fn set_powered(&mut self, powered: bool) -> Result<(), ()> {
        self.adapter().await?.set_powered(powered).await.map_err(|_| ())
    }
}

fn ferrari_model(bytes: &[u8]) -> bool {
    bytes == b"reMarkable Ferrari" || bytes == b"reMarkable Ferrari\0"
}

fn builtin_adapter(directory: &Path, expected_driver: &Path) -> Result<(Option<String>, usize), ()> {
    let entries = match fs::read_dir(directory) {
        Ok(entries) => entries,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok((None, 0)),
        Err(_) => return Err(()),
    };
    let expected_driver = fs::canonicalize(expected_driver).ok();
    let mut builtin = None;
    let mut count = 0;
    for (index, entry) in entries.enumerate() {
        if index >= 32 { return Err(()); }
        let entry = entry.map_err(|_| ())?;
        let name = entry.file_name().into_string().map_err(|_| ())?;
        let Some(number) = name.strip_prefix("hci") else { continue; };
        // Connection and rfcomm class entries can coexist with adapters.
        if number.is_empty() || number.len() > 8 || !number.bytes().all(|c| c.is_ascii_digit()) { continue; }
        count += 1;
        let driver = fs::canonicalize(entry.path().join("device/driver")).ok();
        if expected_driver.is_some() && driver == expected_driver {
            if builtin.replace(name).is_some() { return Err(()); }
        }
    }
    Ok((builtin, count))
}

fn sole_builtin_adapter(directory: &Path, expected_driver: &Path) -> Result<String, ()> {
    let (name, count) = builtin_adapter(directory, expected_driver)?;
    if count != 1 { return Err(()); }
    name.ok_or(())
}

fn write_sysfs(path: &str, bytes: &[u8]) -> Result<(), ()> {
    let mut file = OpenOptions::new().write(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK).open(path).map_err(|_| ())?;
    let metadata = file.metadata().map_err(|_| ())?;
    let mut filesystem = std::mem::MaybeUninit::<libc::statfs>::uninit();
    if unsafe { libc::fstatfs(file.as_raw_fd(), filesystem.as_mut_ptr()) } != 0 { return Err(()); }
    let filesystem = unsafe { filesystem.assume_init() };
    if !trusted_sysfs_node(metadata.mode(), metadata.uid(), filesystem.f_type) { return Err(()); }
    file.write_all(bytes).map_err(|_| ())
}

fn trusted_sysfs_node(mode: u32, uid: u32, filesystem: libc::c_long) -> bool {
    // Stock Ferrari wake nodes are root:500 mode 0660. Group write is valid only
    // after confirming this opened descriptor belongs to the kernel sysfs, where
    // that group cannot replace the node with an ordinary file.
    filesystem == libc::SYSFS_MAGIC && uid == 0
        && mode & libc::S_IFMT == libc::S_IFREG && mode & 0o002 == 0
}

fn acquire_lease(path: &Path, owner: u32) -> Result<fs::File, ()> {
    let file = OpenOptions::new().read(true).write(true).create(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open(path).map_err(|_| ())?;
    let metadata = file.metadata().map_err(|_| ())?;
    if !metadata.is_file() || metadata.uid() != owner || metadata.mode() & 0o777 != 0o600
        || metadata.len() != 0 || metadata.nlink() != 1 { return Err(()); }
    if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(());
    }
    Ok(file)
}

async fn command(program: &str, arguments: &[&str]) -> Result<(), ()> {
    let mut child = Command::new(program).args(arguments)
        .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null())
        .kill_on_drop(true).spawn().map_err(|_| ())?;
    match timeout(Duration::from_secs(3), child.wait()).await {
        Ok(Ok(status)) if status.success() => Ok(()),
        _ => Err(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    #[derive(Default)]
    struct State { calls: Vec<&'static str>, awake: bool }
    struct Fake {
        state: Arc<Mutex<State>>, powered: bool, created: bool,
        fail_prepare: bool, fail_power_on: bool, stall_restore: bool,
    }
    impl Platform for Fake {
        fn wake(&mut self) -> Result<(), ()> {
            let mut state = self.state.lock().unwrap(); state.calls.push("wake"); state.awake = true; Ok(())
        }
        fn unlock(&mut self) {
            let mut state = self.state.lock().unwrap();
            if state.awake { state.calls.push("unlock"); state.awake = false; }
        }
        fn created_adapter(&self) -> bool { self.created }
        async fn prepare(&mut self) -> Result<bool, ()> {
            self.state.lock().unwrap().calls.push("prepare");
            if self.fail_prepare { Err(()) } else { Ok(self.powered) }
        }
        async fn set_powered(&mut self, powered: bool) -> Result<(), ()> {
            self.state.lock().unwrap().calls.push(if powered { "on" } else { "off" });
            if !powered && self.stall_restore { std::future::pending::<()>().await; }
            if powered && self.fail_power_on { Err(()) } else { Ok(()) }
        }
    }
    fn fake(powered: bool) -> (Fake, Arc<Mutex<State>>) {
        let state = Arc::new(Mutex::new(State::default()));
        (Fake { state: state.clone(), powered, created: false, fail_prepare: false,
            fail_power_on: false, stall_restore: false }, state)
    }
    async fn acquire(fake: Fake) -> Result<Guard<Fake>, ()> {
        Guard::acquire(fake, Duration::from_millis(100), Duration::from_millis(20)).await
    }

    #[tokio::test]
    async fn already_powered_adapter_is_not_changed() {
        let (fake, state) = fake(true);
        let mut guard = acquire(fake).await.unwrap(); guard.restore().await; drop(guard);
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "unlock"]);
    }
    #[tokio::test]
    async fn queued_cancellation_prevents_all_hardware_access() {
        let (fake, state) = fake(false);
        let result = Guard::acquire_until(fake, Duration::from_millis(100),
            Duration::from_millis(20), std::future::ready(())).await;
        assert!(result.is_err());
        assert!(state.lock().unwrap().calls.is_empty());
    }
    #[tokio::test]
    async fn unpowered_adapter_is_restored_after_use() {
        let (fake, state) = fake(false);
        let mut guard = acquire(fake).await.unwrap(); guard.restore().await; drop(guard);
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "on", "off", "unlock"]);
    }
    #[tokio::test]
    async fn uncertain_power_on_still_restores() {
        let (mut fake, state) = fake(false); fake.fail_power_on = true;
        assert!(acquire(fake).await.is_err());
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "on", "off", "unlock"]);
    }
    #[tokio::test]
    async fn partial_new_adapter_failure_restores_power_off() {
        let (mut fake, state) = fake(true); fake.created = true; fake.fail_prepare = true;
        assert!(acquire(fake).await.is_err());
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "off", "unlock"]);
    }
    #[tokio::test]
    async fn newly_created_powered_adapter_is_disabled_after_use() {
        let (mut fake, state) = fake(true); fake.created = true;
        let mut guard = acquire(fake).await.unwrap(); guard.restore().await; guard.restore().await;
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "off", "unlock"]);
    }
    #[tokio::test]
    async fn restore_timeout_releases_wake_lock() {
        let (mut fake, state) = fake(false); fake.stall_restore = true;
        let mut guard = acquire(fake).await.unwrap();
        timeout(Duration::from_millis(100), guard.restore()).await.unwrap();
        assert!(!state.lock().unwrap().awake);
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "on", "off", "unlock"]);
    }
    #[tokio::test]
    async fn drop_releases_only_its_wake_lock() {
        let (fake, state) = fake(false); drop(acquire(fake).await.unwrap());
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "on", "unlock"]);
    }
    #[tokio::test]
    async fn cancellation_during_uncertain_power_write_restores() {
        let (mut fake, state) = fake(false);
        fake.stall_restore = true;
        // The cancellation path shares the same bounded restoration as failure.
        let cancelled = async { sleep(Duration::from_millis(5)).await; };
        struct PendingPower(Fake);
        impl Platform for PendingPower {
            fn wake(&mut self) -> Result<(), ()> { self.0.wake() }
            fn unlock(&mut self) { self.0.unlock(); }
            fn created_adapter(&self) -> bool { false }
            async fn prepare(&mut self) -> Result<bool, ()> { self.0.prepare().await }
            async fn set_powered(&mut self, powered: bool) -> Result<(), ()> {
                if powered {
                    self.0.state.lock().unwrap().calls.push("on");
                    std::future::pending().await
                } else { self.0.set_powered(false).await }
            }
        }
        let result = Guard::acquire_until(PendingPower(fake), Duration::from_millis(100),
            Duration::from_millis(20), cancelled).await;
        assert!(result.is_err());
        assert_eq!(state.lock().unwrap().calls, ["wake", "prepare", "on", "off", "unlock"]);
    }
    #[test]
    fn adapter_selection_ignores_connections_and_never_uses_external_adapter() {
        use std::os::unix::fs::symlink;
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let root = std::env::temp_dir().join(format!("rmweb-platform-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
        fs::create_dir(&root).unwrap();
        let adapters = root.join("bluetooth");
        let driver = root.join("btnxpuart");
        let external = root.join("usb");
        fs::create_dir(&adapters).unwrap(); fs::create_dir(&driver).unwrap(); fs::create_dir(&external).unwrap();
        let add = |name: &str, target: &Path| {
            let device = adapters.join(name).join("device");
            fs::create_dir_all(&device).unwrap(); symlink(target, device.join("driver")).unwrap();
        };
        add("hci1", &external); add("hci0:11", &driver);
        assert_eq!(builtin_adapter(&adapters, &driver), Ok((None, 1)));
        assert!(sole_builtin_adapter(&adapters, &driver).is_err());
        add("hci0", &driver);
        assert_eq!(builtin_adapter(&adapters, &driver), Ok((Some("hci0".into()), 2)));
        assert!(sole_builtin_adapter(&adapters, &driver).is_err());
        fs::remove_dir_all(adapters.join("hci1")).unwrap();
        assert_eq!(sole_builtin_adapter(&adapters, &driver), Ok("hci0".into()));
        add("hci2", &driver);
        assert!(builtin_adapter(&adapters, &driver).is_err());
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn model_gate_is_exact() {
        assert!(ferrari_model(b"reMarkable Ferrari\0"));
        assert!(ferrari_model(b"reMarkable Ferrari"));
        for bad in [b"reMarkable Ferrari2".as_slice(), b"reMarkable Ferrari\0trailing", b"reMarkable 2", b""] {
            assert!(!ferrari_model(bad));
        }
    }
    #[test]
    fn stock_group_writable_wake_nodes_require_kernel_sysfs() {
        let stock_mode = libc::S_IFREG | 0o660; // observed uid 0, gid 500
        assert!(trusted_sysfs_node(stock_mode, 0, libc::SYSFS_MAGIC));
        assert!(trusted_sysfs_node(libc::S_IFREG | 0o600, 0, libc::SYSFS_MAGIC));
        assert!(!trusted_sysfs_node(stock_mode, 0, 0));
        assert!(!trusted_sysfs_node(stock_mode, 500, libc::SYSFS_MAGIC));
        assert!(!trusted_sysfs_node(libc::S_IFREG | 0o662, 0, libc::SYSFS_MAGIC));
        assert!(!trusted_sysfs_node(libc::S_IFDIR | 0o660, 0, libc::SYSFS_MAGIC));
    }
    #[test]
    fn ordinary_group_writable_file_is_never_written() {
        use std::os::unix::fs::PermissionsExt;
        let path = std::env::temp_dir().join(format!("rmweb-not-sysfs-{}", std::process::id()));
        let mut file = OpenOptions::new().create_new(true).write(true).mode(0o660).open(&path).unwrap();
        file.write_all(b"preserve").unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o660)).unwrap();
        assert!(write_sysfs(path.to_str().unwrap(), b"changed!").is_err());
        assert_eq!(fs::read(&path).unwrap(), b"preserve");
        fs::remove_file(path).unwrap();
    }
    #[test]
    fn lease_rejects_overlap_and_releases_without_replacing_inode() {
        let root = std::env::temp_dir().join(format!("rmweb-platform-lock-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        let path = root.join("lease");
        let uid = unsafe { libc::geteuid() };
        let first = acquire_lease(&path, uid).unwrap();
        let inode = first.metadata().unwrap().ino();
        assert!(acquire_lease(&path, uid).is_err());
        drop(first);
        let second = acquire_lease(&path, uid).unwrap();
        assert_eq!(second.metadata().unwrap().ino(), inode);
        drop(second);
        fs::write(&path, b"unrelated").unwrap();
        assert!(acquire_lease(&path, uid).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"unrelated");
        let link = root.join("link");
        std::os::unix::fs::symlink(&path, &link).unwrap();
        assert!(acquire_lease(&link, uid).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
