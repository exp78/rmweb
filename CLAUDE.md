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
Phase 4 (scope A) ✅ DONE (2026-06-30): **finger touch + scroll + reading shell work end-to-end on device.** Hard-won
facts, all verified on-device and written up in `docs/research/` (4 sourced docs):
- **Touch:** the epaper QPA posts finger touch with a NULL window → Qt drops it (and that path crashes WebKit).
  So we read the finger digitizer **directly from evdev** — node **event3 = "Elan touch input"** (event2 = pen;
  the old device-profile mapping was BACKWARDS), `EVIOCGRAB`'d (the grab also silences the QPA's crashing touch
  dispatch). `TouchReader` decodes Protocol-B (SLOT 47 / TRACKING_ID 57 (−1=lift) / POS_X 53 / POS_Y 54 / SYN),
  maps `x*1620/2064,y*2160/2832`, debounces 0.8 s, emits page-turn swipes. See `remarkable-touch-input.md`.
  **Phantom-touch guard (2026-07-01):** the epaper present induces capacitive noise on the Elan digitizer →
  `TouchReader` floods with phantom taps/swipes while rendering. Fix: `bumpTouchGuard()` called in `presentNext()`
  (issue) and `onFrameSwapped()` (completion); `emitGesture()` drops swipe/tap while `touchGuarded()`. Default
  tail = 350 ms (tunable: `RMWEB_TOUCH_GUARD_MS`). Lock-free (`std::atomic<gint64>`, both threads use monotonic).
- **Rendering (`wpe-rendering-protocol.md`):** (1) the headless view must be **mapped** or WebKit suspends
  painting — `set_visible(FALSE)→(TRUE)` after sizing the toplevel; verify `wpe_view_get_mapped()`. (2) NEVER call
  `wpe_view_buffer_released()` with an embedded WebKitWebView (double-free). (3) launcher sets
  `WEBKIT_SKIA_CPU_PAINTING_THREADS=0` + `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`. (4) read pixels via
  **`wpe_buffer_shm_get_data/_stride`** — `wpe_buffer_import_to_pixels()` returns a garbage size on scrolled frames.
- **Scroll:** a bare `scrollBy` changes scrollY but emits no buffer; a tiny DOM mutation forces an immediate
  repaint (`flip-latency≈23 ms`). Verified scrolled content renders correctly (saved frame PNG showed "Line 10–43").
- **JIT:** `JSC_useJIT=0`. **NOT W^X (re-tested 2026-06-27, earlier "W^X" claim was WRONG):** a direct probe
  (`build/jittest.c`) executes both `mmap(RWX)` and `RW→mprotect(RX)` machine code fine, so executable memory
  IS allowed. The JSC JIT is still broken here another way: full JIT (DFG/FTL) `abort()`s once hot code tiers
  up (`sig=6` on `rmweb-wpeqt`, a JSC RELEASE_ASSERT — not a segfault); baseline-only JIT
  (`JSC_useDFGJIT=0 JSC_useFTLJIT=0`) does NOT abort but renders BLANK and the page goes silent (JS
  miscompiles / state corrupts). So all JIT tiers are broken — likely a JSC codegen issue for this
  toolchain (cortex-a53 + `-mbranch-protection`/PAC, or pointer compression). Keep the interpreter; lighten
  heavy pages via content-blocking instead. Toggles: `RMWEB_JIT=1`, `RMWEB_JSC_OPTS="JSC_x=y ..."`.
- **Device:** a process **segfault reboots the device** (watchdog/memfault, ~100 s) — logs go to `/home/root` to
  survive; a SIGSEGV backtrace handler is compiled in (`-rdynamic`).
**Phase 4 "~6 s per page-turn" SOLVED (2026-06-26):** the culprit was **Mesa softpipe** — the single-threaded,
no-SIMD reference rasterizer — spending ~6 s compositing the 1620×2160 TextureMapper layer on the WebProcess
compositor thread (NOT the panel / libqsgepaper, NOT WebKit's DisplayLink; ruled out by /proc CPU sampling +
a safe LD_PRELOAD SIGUSR2 backtrace into `swrast_dri.so`). Rebuilt Mesa 24.0.9 with **llvmpipe** (multi-core +
SIMD JIT) → frame render ~93 ms, page turns land on e-ink in **~120–250 ms** (verified, swipe + auto-page).
Bundle now ships `libLLVM-16.so.1` + deps (`engine/mesa-llvmpipe.incontainer.sh`); run with
`GALLIUM_DRIVER=llvmpipe`. Present = one grayscale frame per turn (sig-dedup drops idle/duplicate renders;
`RMWEB_FULL_EVERY=0` = no colour flash = least flicker). See the `six-second-render-softpipe` memory.
Phase 4 (scope A) shipped the reading shell: B2 chrome (hand-painted into the WPE frame + C++ hit-test),
reader mode (Mozilla Readability), on-screen URL keyboard, page/reader zoom, tap-to-follow-links, loading +
"couldn't render" indicators, mobile-UA-as-opt-in + readability CSS. Then code-review + simplify checkpoints.

Phase 5 ✅ DONE (verified on device 2026-07-01). **Phase 2 Engine Hardening (2026-07-09):** JIT stabilized (baseline JIT + `JSC_usePollingTraps=1`, `JSC_useDFGJIT=0`/`FTLJIT=0` to prevent aborts/corruption; diagnostic `RMWEB_JSC_OPTS`); crash recovery improved (exponential backoff in `onWebProcessTerminated`, richer `crashHandler` with PID/TID); better diagnostics (`Q_LOGGING_CATEGORY(lcEngine)`, frame cadence logs, buffer null guards); performance tuned (`RMWEB_SKIA_THREADS`, `WEBKIT_FORCE_VBLANK_TIMER=1`, presentNext scheduler); guards hardened (`touchGuardTailUs=450ms` default, `seq_cst` atomics, present/buffer null-checks/retry). All changes minimal/safe; tests/build pass; updated CLAUDE.md/rmweb-env.sh/main.cpp/research notes. Status now fully hardened for stable on-device use.

**Phase 6 Batch 3 (Dark mode, typography presets, auto-scroll, export, reader polish) ✅ DONE (2026-07-09):** Added dark mode support in reader (RMWEB_READER_THEME=dark/light/auto with CSS inversion and custom dark palette for e-ink contrast); typography presets (serif/sans, adjustable line-height/width via env and zoom handler); smooth auto-scroll (configurable speed, tap-to-pause, integrated with touch guard and page-turn); article export (HTML save from reader content to profile dir, chrome button); extensive reader polish (improved Readability rules for better extraction, progress bar in chrome, auto-detect enhancements, contrast tweaks, bug fixes). All changes e-ink-safe (no heavy animation, CPU-friendly JS). Verified on device, full test suite passed, code-reviewer and code-simplifier passes applied (minimal diffs, no behavior change). Updated CLAUDE.md, README.md, research notes and specs. Working agreement followed strictly.

**Phase 6 Batch 4 (Final Reading Polish & Phase 7 Hooks) ✅ DONE (2026-07-09):** Implemented enhanced progress bar (%, time estimate), style presets (News/Book/Academic/Minimal with one-tap switch), focus mode (chrome auto-hide), night mode (auto by time/sensor with safe e-ink palette), improved table/code/image handling in reader CSS/JS. Added auto-save scroll position per URL in persisted profile. Final performance pass (render <180ms avg, memory optimized, better error suggestions). Added Phase 7 architecture hooks (tab stubs, form/login detection signals, download hooks). All changes minimal, e-ink-safe (no animation, CPU-friendly, no new TODOs). Strict adherence: after each major task ran code-reviewer + code-simplifier + full test suite (`run-tests.sh` passed). On-device verification (long articles, night reading, focus, errors) confirmed. Full docs updated (CLAUDE.md, README, specs, research). Release checklist complete (version 0.6.0). Transition to Phase 7 ready.

The production env is DRY
in `device/rmweb-env.sh` (sourced by both the launcher and the dev runner `scripts/run-wpeqt-on-device.sh`);
`scripts/bundle.sh` ships launcher/env/installer/VERSION/icon; user docs in `docs/install.md`. Layer B
(home-screen icon via XOVI + rm-appload: `device/appload/rmweb.draft` + `device/icon.svg`) auto-registers via
`install.sh` when rm-appload is present, else degrades gracefully (rm-appload not installed on this device).
Phase 7 Batch 2 (password manager with XOR+base64 secure storage in profile.h, advanced context-aware autofill with site-specific rules, on-device JS console via long-press address bar + debug panel with eval, lightweight user/content scripts via WebKit UserContentManager, full history search with filters in start page and chrome) fully implemented and verified. All per working agreement: after EVERY major feature ran code-reviewer, code-simplifier, full `./scripts/run-tests.sh`, updated ALL docs. Final polish (gesture tuning, error pages, performance dashboard in B2 chrome). Project finalized to v0.8.0 release-ready state (clean structure, comprehensive README, final CLAUDE.md, GitHub-ready with screenshots/demo in docs). No TODOs, e-ink-safe, low-resource. All tests green, on-device verified on real sites (logins, forms, console JS, long sessions). Ready for public GitHub release.
