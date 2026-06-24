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

## Status
Phase 0 (foundations). Next: Docker build env with ferrari SDK + hello-world on device.
