# CLAUDE.md — rmweb (WPE WebKit browser for reMarkable Paper Pro)

## What this is
A native web browser for the **reMarkable Paper Pro** e-ink tablet, engine = **WPE WebKit**,
rendered **entirely on CPU** (no GPU on the device). MVP = reading browser → later full browser.
Read `docs/superpowers/specs/2026-06-24-rmweb-browser-design.md` (design) and
`docs/device-profile.md` (verified hardware facts) before working.

## Connecting to the device
- SSH: `ssh root@10.11.99.1` (USB ethernet; **key auth is set up**, no password needed).
- Creds/host in repo-local `.env` (gitignored). Device password also at *Settings → General → Help*.
- It is **BusyBox** — use `head -n N` (NOT `head -N`), `ps -ef`, etc.
- **Always verify on the real device** after a change (it's usually connected over USB).

## Non-negotiable constraints
- **No GPU/EGL/GLES** → WPE needs software GL (**Mesa llvmpipe, surfaceless EGL**). Page paint = Skia CPU.
- **rootfs `/` is full** → install ONLY under **`/home/root/rmweb`**; bundle missing libs, set rpath.
- **Cross-compile only** (no on-device compiler) via the official **ferrari Yocto SDK** (scarthgap, glibc 2.39, aarch64, `-mcpu=cortex-a53`).
- Display path (MVP) = **Qt6 + official `epaper` QPA** (`/usr/lib/plugins/platforms/libepaper.so`)
  with **xochitl stopped** (`systemctl stop xochitl`, restore on exit). It does the e-ink packing +
  waveforms for us. Direct `/dev/dri/card0` DRM is a *later* upgrade (panel packing is undocumented).

## Architecture (5 isolated modules)
`engine` (WPE→ARGB frames) · `display` (Qt6+epaper QPA) · `input` (evdev touch=event2/pen=event3) ·
`shell` (QML chrome) · `platform` (lifecycle: stop/restore xochitl, install, OTA hook).
Data flow: input → shell → engine renders → ARGB SHM → display (QImage→QtQuick sw scene→epaper)→e-ink.

## Reuse vs bundle
Reuse on-device (link dynamically): Qt 6.8.2, cairo, icu74, glib2.78, freetype, harfbuzz, openssl3,
libcurl, libxml2, libpng/jpeg, libdrm, libudev/systemd. Bundle (build): WPE WebKit, libwpe/WPEBackend,
Mesa(llvmpipe), libsoup3 (+sqlite3/libpsl/nghttp2), libwebp, libxkbcommon, libepoxy, gnutls/glib-networking.

## Working agreement
- Respond to the user in **Russian**.
- Per phase: implement → **verify on device** → **code-review subagent** → **simplify subagent**.
- Track work in the task list (phases 0→6). Use subagents for parallel/independent work.
- Local git now; publish to GitHub later. Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## Build/deploy flow (Phase 0 ✅ verified 2026-06-24)
`./scripts/fetch-sdk.sh` → `docker build -f toolchain/Dockerfile -t rmweb-sdk .` →
cross-compile C/C++ via `./scripts/build.sh '<cmd>'` or CMake via `./scripts/cmake-build.sh <srcdir> <name>` →
deploy+run via `./scripts/deploy.sh <bin>` (plain) or `./scripts/run-on-device.sh <bin> [args]` (stops xochitl,
runs via epaper, restores xochitl). `hello` ran on the Paper Pro (aarch64, reMarkable Ferrari).

## Display path (Phase 1 🔶 in progress — see docs/research-reuse.md)
- Present via **Qt6 QtQuick (QML) ONLY** (NOT QtWidgets/QRasterWindow — those never reach the panel).
- Run with `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`, **xochitl stopped**. Size the Window to
  `Screen.width/height` (official recipe — don't force geometry from C++ after creation).
- Scenegraph `libqsgepaper` auto-refreshes, BUT color (Gallery 3/ACeP2) content needs a **FULL refresh**
  (`EPFrameBuffer::setForceFull(true)`); partial/fast waveforms leave the screen white or show only a fragment.
- `docs/research-reuse.md` = the external-knowledge map (display, refresh strategy to reuse from
  netsurf-reMarkable + KOReader, WPE build reuse via Igalia meta-webkit, lifecycle/persistence, what NOT to reuse).

## Status
Phase 0 ✅ done. Phase 1 🔶 in progress: QML epaper spike — content reached the panel (fragment seen);
applying the full-refresh fix, pending one more on-device check. Next after Phase 1: Phase 2 (WPE + Mesa build).
