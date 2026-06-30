# rmweb — Phase 5: Packaging & Launch (design)

> Status: **approved 2026-06-30** (brainstorming). Next: implementation plan via writing-plans.
> Scope decisions locked by the user: **A + B launch** (self-contained launcher *and* an rm-appload
> icon on top); **MVP stability** (reliable xochitl restore + no-brick + reboot/OTA survival + in-app
> exit; suspend/resume deferred).

## Goal

Turn rmweb from a dev script driven over SSH from the host into a self-contained, installable app that
lives entirely under `/home/root/rmweb`, **launches on the device itself**, **always restores xochitl**
(never bricks the home UI), survives reboot/OTA, and — as a second layer — appears as a tappable icon in
the device UI via XOVI + rm-appload.

## Architecture

Strategy **A (standalone)** is the foundation and is fully in our control: a hardened on-device launcher
script plus an idempotent installer, everything under `/home/root/rmweb`, touching nothing in `/etc` or
rootfs. Strategy **B (rm-appload icon)** is a thin layer on top: a small app descriptor + icon whose
`exec` simply calls the Strategy-A launcher. If the community launcher (XOVI/rm-appload) is absent or
lost to an OTA, Strategy A still gives a complete app.

## Constraints (inherited, verbatim)

- Install **only** under `/home/root/rmweb` (rootfs `/` is full / read-only). Bundle missing libs, set rpath.
- Device is **BusyBox**: no `timeout`, no `flock`, no `pkill`; use `head -n N`, `mkdir` for atomic locks, `kill -0` loops.
- Display path = Qt6 epaper QPA with **xochitl stopped** (`systemctl stop xochitl`, restore on exit).
- Never **disable** xochitl: it must run at boot to unlock the LUKS `/home` partition. We only `stop`/`start` it.
- Production env already proven (from `scripts/run-wpeqt-on-device.sh` show mode): `GALLIUM_DRIVER=llvmpipe`,
  software EGL surfaceless, `WEBKIT_*` CPU rendering flags, `JSC_useJIT=0`, `QT_QPA_PLATFORM=epaper`.

## On-device layout (extends the existing bundle)

```
/home/root/rmweb/
  bin/rmweb-wpeqt        # the app (already bundled by scripts/bundle.sh)
  lib/ libexec/ share/ qml/ plugins/   # runtime (already bundled)
  rmweb                  # NEW: production launcher — runs ON the device
  rmweb-env.sh           # NEW: single source of truth for the production env list
  install.sh             # NEW: idempotent installer / OTA re-hook
  icon.png               # NEW: app icon (layer B)
  appload/               # NEW: rm-appload descriptor (layer B)
  VERSION                # NEW: bundle version stamp
  rmweb.log              # runtime log (under /home — survives a watchdog reboot)
```

## Components (each a single, isolated responsibility)

### 1. `device/rmweb` — production launcher (runs on the device)
Distilled from the `show` path of `scripts/run-wpeqt-on-device.sh`, production-only, hardened.

Responsibilities, in order:
1. Acquire a single-instance lock: `mkdir /home/root/rmweb/.lock` (atomic on BusyBox; fail fast if it exists).
2. If `systemctl is-active xochitl` → `systemctl stop xochitl`; record `STOPPED=1` only when we actually stopped it.
3. Overlay-mount `/usr/libexec` (WPE's baked helper prefix; `/` is read-only):
   `mount -t overlay overlay -o lowerdir=/usr/libexec,upperdir=$R/ovl/upper,workdir=$R/ovl/work /usr/libexec`; record `MOUNTED=1`.
4. `. /home/root/rmweb/rmweb-env.sh` (the shared env list).
5. `exec rmweb-wpeqt "$@"` with stdout/stderr → `/home/root/rmweb/rmweb.log`.

**No-brick teardown** — `trap` on `EXIT INT TERM` runs the cleanup on *every* exit path (clean quit, app
crash, launcher TERM). Cleanup order makes xochitl restore independent of the umount:
1. restart xochitl **if** `STOPPED=1` (do this first / unconditionally of umount success);
2. `umount /usr/libexec` if `MOUNTED=1`;
3. `rmdir /home/root/rmweb/.lock`.

`set -u`. SIGKILL (`kill -9`) cannot be trapped — covered by the boot-enabled-xochitl backstop (see below).

**Consumes:** `rmweb-env.sh`, `bin/rmweb-wpeqt`. **Produces:** `rmweb.log`; exit code = app exit code.

### 2. `device/rmweb-env.sh` — shared production env
A sourced fragment exporting the proven production env (LD_LIBRARY_PATH, `GALLIUM_DRIVER=llvmpipe`,
`LIBGL_ALWAYS_SOFTWARE=1`, `EGL_PLATFORM=surfaceless`, `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`,
`WEBKIT_INJECTED_BUNDLE_PATH`, `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`, `WEBKIT_SKIA_CPU_PAINTING_THREADS=0`,
`WEBKIT_DISABLE_ASYNC_SCROLLING=1`, `JSC_useJIT=0`, `QT_QPA_PLATFORM=epaper`, `QT_QUICK_BACKEND=epaper`,
`QT_IM_MODULE=qtvirtualkeyboard`, `QML_IMPORT_PATH`/`QT_PLUGIN_PATH` extended not replaced). Both the
on-device launcher and the dev `run-wpeqt-on-device.sh show` path read from here → no drift.

**Consumes:** nothing. **Produces:** exported env for the launcher.

### 3. `device/install.sh` — idempotent installer / OTA re-hook (runs on the device)
1. Integrity check: assert `bin/rmweb-wpeqt`, `rmweb`, `rmweb-env.sh`, key `lib/` pieces exist; non-zero exit + clear message if not.
2. `chmod +x rmweb install.sh`.
3. Write `VERSION`.
4. Layer B: copy `appload/` descriptor + `icon.png` into the rm-appload apps directory **if rm-appload is present** (skip with a notice otherwise).
5. Print next-steps (how to launch; how to recover after OTA).

Idempotent — re-running is safe and is the documented OTA-recovery procedure.

**Consumes:** the pushed bundle. **Produces:** executable launcher, registered icon (when rm-appload present), `VERSION`.

### 4. In-app exit to menu (UX)
A `⏻` button in the B2 chrome bar (painted into the WPE frame + C++ hit-tested, exactly like the Reader
button). The tap handler calls **`_exit(0)`** — an immediate exit that skips WebKit's static destructors,
deliberately avoiding the known clean-exit `SIGABRT` in WebKit teardown (see the
`webkit-clean-exit-abort-save-mode` note). `_exit(0)` is a normal exit (code 0, no signal) → no
watchdog/memfault reboot. The launcher's `trap` then restores xochitl.

**Consumes:** the existing chrome hit-test + tap router. **Produces:** process exit 0 → launcher restores the home UI.

### 5. `device/appload/` descriptor + `device/icon.png` — rm-appload entry (layer B)
A minimal rm-appload app descriptor: name `rmweb`, icon `icon.png`, `exec = /home/root/rmweb/rmweb`.
This is the only piece that depends on third-party tooling and **requires on-device verification** (the
exact apps-directory path and descriptor format are confirmed when the device is online). Degrades
gracefully: without rm-appload, Strategy A is unaffected.

## Lifecycle / data flow

```
tap icon (rm-appload)  ─┐
SSH: /home/root/rmweb/rmweb ─┤─►  launcher: lock → stop xochitl → overlay-mount /usr/libexec
                          → source rmweb-env.sh → exec rmweb-wpeqt   (logs → rmweb.log)
app runs ─► user reads/browses ─► tap "⏻" ─► _exit(0)
trap (fires on ANY exit: clean / crash / TERM):
                          → restart xochitl (if we stopped it) → umount overlay → release lock
```

**No-brick guarantee.** xochitl is restored on every trappable exit path. Backstop for the untrappable
case (launcher `kill -9`): we never `disable` xochitl, so it is boot-enabled — a reboot always brings the
home UI back. MVP relies on trap-restore + this boot backstop; no extra watchdog needed.

## Reboot / OTA survival

- Everything under `/home/root/rmweb` → `/home` is LUKS-persistent → survives reboot **and** OTA. ✓
- Strategy A writes nothing to `/etc` or rootfs → nothing to lose on OTA. ✓
- rm-appload (layer B) is community tooling and may be wiped by an OTA. Recovery: re-run `install.sh` to
  re-assert our descriptor; reinstall XOVI/rm-appload if the icon is gone. Documented in the user docs.

## Error handling

- **Launcher:** `set -u`; `trap` restores state on `EXIT/INT/TERM`; atomic `mkdir` lock prevents a second
  instance double-stopping xochitl; all output to `rmweb.log`.
- **App:** existing crash handler (SIGSEGV/SIGABRT backtrace) + WebProcess auto-reload (2×) retained;
  exit path uses `_exit(0)` to dodge the WebKit teardown abort.
- **Installer:** idempotent; verifies key files; clear next-steps; non-zero exit on missing pieces.

## Testing

- **Host tests (no device, runnable in CI):**
  - `shellcheck` the launcher, env fragment, and installer.
  - A stub harness on the host: PATH-shimmed fake `systemctl` / `mount` / `umount` / a fake app binary,
    asserting the no-brick contract — stops xochitl, mounts the overlay, and **restores xochitl on clean
    exit, on a simulated crash (app exits non-zero), and on `TERM`**; the lock prevents a double-stop;
    no double-stop when xochitl was already inactive.
- **On-device verification (when online):** push bundle → `install.sh` → `/home/root/rmweb/rmweb` brings
  the browser up on e-ink → `⏻` returns to xochitl → reboot: xochitl normal + bundle intact + relaunch
  works → (layer B) the icon appears in rm-appload and launches the app.
- Then, per the working agreement: **code-review subagent → simplify subagent**.

## Out of scope (now)

- **Suspend/resume** (device sleep): the docs flag it "fragile/undocumented"; deferred to a later step.
  While the app is foreground with xochitl stopped, the device does not auto-sleep.
- **GitHub publish:** outward-facing; requires an explicit go-ahead and a careful `.env` guard (it holds
  the device password). Tracked separately, not bundled into this packaging work.
- Dev script `scripts/run-wpeqt-on-device.sh` keeps its `save`/`bench`/debug modes; its `show` path is
  re-pointed to delegate to the on-device `device/rmweb` (DRY, no env drift).

## Items requiring on-device confirmation (carry into the plan)

- rm-appload apps-directory path + descriptor schema on this firmware (verify when online).
- Whether `systemctl is-active xochitl` / overlay mount behave identically from an on-device shell as from
  the host-driven SSH script (expected yes; the launch logic is the same, only the invocation site moves).
