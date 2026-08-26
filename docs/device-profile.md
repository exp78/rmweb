# reMarkable Paper Pro — Device Profile (verified)

All facts below were verified over SSH on **2026-06-24** against the live device;
versioned facts re-verified **2026-08-26** after an OTA to reMarkable OS 3.28.0.164.
This is the ground truth that drives every architecture decision. Re-verify after
any firmware update (especially storage, glibc, kernel, and xochitl offsets).

## Connection

- Transport: **USB ethernet**. Device IP **`10.11.99.1`**; host interface `en12` gets `10.11.99.5/27`.
- SSH: `root@10.11.99.1`, **key auth configured** (our `~/.ssh/id_ed25519.pub` is in the device's `authorized_keys`).
- Root password is stored in repo-local `.env` (gitignored). Find it on-device at
  *Settings → General → Help → Copyrights and licenses*.
- Ping RTT ~1.45 ms.

## SoC / CPU

- **NXP i.MX8M Mini** (`/sys/devices/soc0/soc_id` = `i.MX8MM`), machine = `reMarkable Ferrari`.
- **4× ARM Cortex-A53** (`CPU part 0xd03`), **aarch64**.
- Features: `fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid`. `nproc` = 4.

## OS / Kernel / ABI

- **Codex Linux 5.8.199** — Yocto/OpenEmbedded **scarthgap** (OE 5.0 LTS).
- Firmware image version **3.28.0.164** (re-verified 2026-08-26; was 3.27.1.0 at the 2026-06-24
  check, `/etc/version` = `20260506100933`).
- Kernel **Linux 6.12.49** (`#1 SMP PREEMPT`), hostname `imx8mm-ferrari`.
- **glibc 2.39** (scarthgap). Loader `/lib/ld-linux-aarch64.so.1`, `/lib/libc.so.6`.
- Userland is **BusyBox** (`v1.36.1`) — note: `head -N` is NOT supported, use `head -n N`.

## Memory / Storage

- RAM **~2.0 GB** + **~2.5 GB swap**.
- **`/` (rootfs) = 500 MB, ~91% full (~42 MB free)** → **NEVER install here.**
- **`/home` = 46 GB encrypted (`/dev/mapper/home-encrypted-disk`), ~41 GB free** → **install target.**
  Install everything under **`/home/root/rmweb`**.
- `/etc`, `/var/lib`, `/var/cache`, `/srv` are 981 MB overlays. `/data` = `mmcblk0p1` (88 MB).
- OTA updates wipe rootfs changes → install under `/home` + provide a re-install/after-update hook.

## Display — the hard part

- **No legacy framebuffer** (`/dev/fb*` absent).
- **DRM/KMS** via kernel driver **`imx-drm`**, node **`/dev/dri/card0`** (display controller only).
- Connector **`card0-LVDS-1`**, status `connected`, single mode **`405x1084`**.
- The panel is **E Ink Gallery 3, real resolution 1620×2160, 32-bit ARGB8888** (color e-ink).
  `405 × 4 = 1620` → the DRM mode is a **packed transport**; the 405×1084→1620×2160 packing
  is **undocumented / not publicly reverse-engineered** (lives inside closed `libepaper.so` +
  kernel panel/DTS). This is why we avoid direct DRM at first.

## GPU — present on die, but no stock driver

- **No DRM render node** (`/dev/dri/renderD128` absent) — the GPU is not exposed to userland.
- No GPU kernel modules (no `etnaviv`, no `galcore`/Vivante), nothing in dmesg, no `/dev/galcore`.
- **No EGL / GLES / Vulkan / GBM / Wayland** anywhere on the FS — only `libdrm` is present.
- The i.MX8M Mini silicon **does** contain a **Vivante GC7000 UltraLite** GPU, but the stock OS ships
  **no driver** for it. **All rendering is CPU in practice.** (WPE therefore needs a *software* GL — Mesa llvmpipe.)

## UI stack (what reMarkable itself does)

- **`/usr/bin/xochitl`** (24 MB) — **Qt 6.10.3 Quick/QML** app, **active** (pid varies).
- Renders **on CPU** (no GPU among its ~100 loaded libs) and presents via a **custom Qt platform
  plugin `/usr/lib/plugins/platforms/libepaper.so`** ("epaper" QPA) → `imx-drm`.
- Only stock QPA plugins present besides epaper: `offscreen`, `minimal`, `vnc`.
- **We mirror this model**: CPU-render → epaper QPA → e-ink.

## Input devices

- `/dev/input/event0` — `30370000.snvs:snvs-powerkey` (power button)
- `/dev/input/event1` — `Hall effect sensors` (folio cover)
- `/dev/input/event2` — `Elan marker input` (**pen / stylus**)
- `/dev/input/event3` — `Elan touch input` (**finger touchscreen**)
  - ⚠️ CORRECTED 2026-06-26 (was reversed above): on the Paper Pro **event2 = PEN, event3 = TOUCH**
    (the opposite of the rM2-era convention). **Resolve by NAME via `EVIOCGNAME`, never by `eventN`.**
    Finger touch = Elan Type-B multitouch, `ABS_MT_POSITION_X` 0..2064, `ABS_MT_POSITION_Y` 0..2832,
    `INPUT_PROP_DIRECT`. See `docs/research/remarkable-touch-input.md` for the full input plan.

## On-device libraries

**Reusable (link dynamically, do NOT bundle):** Qt 6.10.3 (Core/Gui/Qml/Quick/QuickControls2/
Svg/Network/DBus/WebSockets/Xml…), cairo 1.18, pixman, freetype 2.13 (`.6.20.1`), fontconfig,
harfbuzz (+`-cairo`,`-gobject`), **icu 74**, glib/gio/gobject/gmodule 2.78, libpng16, libjpeg62,
libxml2, libcurl, **openssl 3** (libssl/libcrypto), libgcrypt, libsystemd, libudev, **libdrm**.

**Missing → must cross-build & bundle under `/home`:**
WPE WebKit (`libWPEWebKit`), `libwpe` + `WPEBackend` (or WPEPlatform), **Mesa** (llvmpipe +
software EGL/GLESv2 + surfaceless), **libsoup3** (+ sqlite3, libpsl, libnghttp2),
**libwebp** (required by WPE), **libxkbcommon**, libepoxy, a TLS backend for glib-networking
(GnuTLS+libtasn1, or configure for openssl). **No compiler on device** → cross-compile only.

## Toolchain

- **Official reMarkable "ferrari" Yocto SDK** matches this device:
  `https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/ferrari/remarkable-production-image-5.7.119-ferrari-public-{x86_64,aarch64}-toolchain.sh`
  (3.27.0.97 remains ABI-compatible with the device's 3.28.x — same scarthgap/glibc 2.39.)
- Triple `aarch64-remarkable-linux`, tune `-mcpu=cortex-a53`, sysroot
  `cortexa53-crypto-remarkable-linux`. Toltec/rM2 toolchains are ARMv7 → **unusable**.
