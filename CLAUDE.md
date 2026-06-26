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
Phase 0 ✅ done. Phase 1 ✅ DONE & verified on device (2026-06-25): a standalone Qt6 QML app presents a full
test pattern on the Paper Pro e-ink via the epaper QPA (cure = Window sized to `Screen.width/height`; QtQuick
only; `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`; xochitl stopped/restored). The real rMPP refresh API
is recorded for Phase 4 (`EPFramebuffer::swapBuffers/ghostControl`, exported by libqsgepaper — see research-reuse.md).
Phase 2 ✅ DONE (2026-06-25): **WPE WebKit 2.48.5 (Skia CPU, software) cross-built** for aarch64 + **Mesa softpipe**
(software EGL, surfaceless, no GPU); a headless `engine/wpe_render.c` **rendered a real web page to PNG**
(`build/wpe-render.png` — bar + colored boxes + anti-aliased text). Recipe: `scripts/build-wpe.sh {deps|build|render}`
→ `engine/*.incontainer.sh` (builds on a persistent, case-sensitive docker volume → resumable); all gotchas in
research-reuse.md §8 (sysroot pkg-config, glibc-2.39 loader repoint, `/usr` symlinks, `load_html`, fonts).
Phase 3 ✅ DONE (2026-06-25): a **web page rendered by WPE WebKit is shown on the Paper Pro e-ink**. `engine/wpeqt`
is a Qt6 app (= WPE UIProcess); `WpeEngine` drives WPE headless on a worker thread → `buffer-rendered` BGRA → `QImage`
→ `QQuickPaintedItem` → epaper QPA (xochitl stopped). Device bundle = `/home/root/rmweb` (`scripts/bundle.sh`);
build `scripts/build-wpeqt.sh`; run `scripts/run-wpeqt-on-device.sh {save|show}`. Engine also proven standalone
on-device (3a: `scripts/render-on-device.sh`). All integration gotchas in research-reuse.md §8 (QT_NO_KEYWORDS,
worker-thread GMainContext, BGRA==ARGB32, /usr/libexec overlay, BusyBox no-timeout).
Phase 4 (scope A) 🔶 IN PROGRESS (2026-06-26): **finger touch + scroll work end-to-end on device.** Hard-won
facts, all verified on-device and written up in `docs/research/` (4 sourced docs):
- **Touch:** the epaper QPA posts finger touch with a NULL window → Qt drops it (and that path crashes WebKit).
  So we read the finger digitizer **directly from evdev** — node **event3 = "Elan touch input"** (event2 = pen;
  the old device-profile mapping was BACKWARDS), `EVIOCGRAB`'d (the grab also silences the QPA's crashing touch
  dispatch). `TouchReader` decodes Protocol-B (SLOT 47 / TRACKING_ID 57 (−1=lift) / POS_X 53 / POS_Y 54 / SYN),
  maps `x*1620/2064,y*2160/2832`, debounces 0.8 s, emits page-turn swipes. See `remarkable-touch-input.md`.
- **Rendering (`wpe-rendering-protocol.md`):** (1) the headless view must be **mapped** or WebKit suspends
  painting — `set_visible(FALSE)→(TRUE)` after sizing the toplevel; verify `wpe_view_get_mapped()`. (2) NEVER call
  `wpe_view_buffer_released()` with an embedded WebKitWebView (double-free). (3) launcher sets
  `WEBKIT_SKIA_CPU_PAINTING_THREADS=0` + `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`. (4) read pixels via
  **`wpe_buffer_shm_get_data/_stride`** — `wpe_buffer_import_to_pixels()` returns a garbage size on scrolled frames.
- **Scroll:** a bare `scrollBy` changes scrollY but emits no buffer; a tiny DOM mutation forces an immediate
  repaint (`flip-latency≈23 ms`). Verified scrolled content renders correctly (saved frame PNG showed "Line 10–43").
- **JIT:** `JSC_useJIT=0` (a JS page segfaulted with the JIT — revisit: rMPP may not actually be W^X).
- **Device:** a process **segfault reboots the device** (watchdog/memfault, ~100 s) — logs go to `/home/root` to
  survive; a SIGSEGV backtrace handler is compiled in (`-rdynamic`).
Remaining for Phase 4: **e-ink refresh tuning** (screen updates ~every 6 s, not per page-turn — needs explicit
full refresh via libqsgepaper `EPFramebuffer`), reading-shell chrome, HTTPS bundling, then code-review + simplify.
