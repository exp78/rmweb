# rmweb — a WPE WebKit browser for the reMarkable Paper Pro

**Status:** approved design (2026-06-24) · **Scope now:** reading (A) · **Scope later:** full browser (B)
**Working name:** `rmweb` (provisional, rename before GitHub publish if desired)

## 1. Purpose

A web browser that runs natively on the reMarkable Paper Pro e-ink tablet, built on the
**WPE WebKit** engine. We start as a fast, high-contrast **reading browser** (articles, docs,
wikis, long-form) and grow toward a **general-purpose browser** within the hardware limits.

## 2. Constraints (from `docs/device-profile.md`)

- aarch64, 4× Cortex-A53, ~2 GB RAM + 2.5 GB swap. **CPU-only in practice — the SoC has a GPU
  (Vivante GC7000 UltraLite), but the stock OS ships no driver for it, so no GPU/EGL/GLES is usable.**
- Yocto scarthgap, glibc 2.39. Cross-compile only (no on-device toolchain).
- Display is `imx-drm` e-ink, real 1620×2160 ARGB8888, exposed as a **packed** `405×1084` DRM
  mode whose packing is **undocumented**. Refresh is slow with ghosting (e-ink physics).
- rootfs is full → **install under `/home/root/rmweb`**, bundle missing libs with rpath.
- Many WebKit deps already on device (cairo, icu74, glib2.78, freetype, harfbuzz, openssl3,
  libdrm, full Qt 6.10.3) → reuse; bundle only what's missing.

## 3. Key technical decisions

### 3.1 Engine: WPE WebKit, software rendering
- Page paint: **Skia CPU raster** (WPE default since 2.46).
- Compositor: WPE's compositor is **mandatory and EGL-based** — it cannot be removed. We satisfy
  it with **Mesa llvmpipe + surfaceless EGL** (software GL on CPU). "Software-only" = software *GL*.
- Output: WPE hands us **ARGB8888 buffers in shared memory** per frame, via either **WPEPlatform
  headless** (`wpe_display_headless_new`, `WPEBufferSHM`) or classic **WPEBackend-fdo exportable**
  (`export_shm_buffer`). We pick one in the Phase-2 spike (fdo is more battle-tested today;
  WPEPlatform is cleaner but was preview through ~2.52 — verify current status at build time).
- Build slim: `USE_GSTREAMER=OFF` (no media), `ENABLE_WEBGL=OFF`, no WebRTC/spellcheck/gamepad,
  etc. Must-have deps: libsoup3 (+sqlite3/libpsl/nghttp2), libwebp, libxkbcommon, libepoxy,
  Mesa EGL/GLES, TLS for glib-networking.

### 3.2 Display: official `epaper` QPA (chosen path A)
- **Decision:** MVP presents through reMarkable's on-device **`libepaper.so` (epaper QPA)** — it
  does the undocumented panel packing **and** waveform/refresh for us. We run **with `xochitl`
  stopped** (`systemctl stop xochitl`), our process becoming DRM master, and restore on exit.
- A minimal **Qt 6 (QML) host** owns the screen via `-platform epaper`; the web content arrives as
  a `QImage` (from WPE's ARGB buffer) drawn fullscreen, with browser chrome composed around it in
  a software QtQuick scene.
- **Rejected for MVP:** (B) `LD_PRELOAD` hook into xochitl — needs re-deriving firmware-specific
  offsets, fragile across updates. (C) direct `/dev/dri/card0` dumb buffers — needs reversing the
  undocumented packing. **(C) is the planned later upgrade** once we want finer refresh control.

### 3.3 Install & lifecycle
- Everything under `/home/root/rmweb` (binary + bundled libs), `RPATH=$ORIGIN/../lib` /
  `LD_LIBRARY_PATH`. Launcher stops/restores xochitl, survives reboot, re-installs after OTA.

## 4. Architecture

Five isolated modules, each independently testable, communicating over narrow interfaces:

| Module | Responsibility | Depends on | Does NOT know about |
|---|---|---|---|
| `engine`  | Embed WPE; load URLs; produce ARGB frames; expose input sink | WPE, Mesa(sw GL), libsoup | e-ink, Qt, packing |
| `display` | Own the screen via Qt6 + epaper QPA; blit ARGB `QImage`; pick refresh mode | Qt6, libepaper.so | the web |
| `input`   | evdev `event3`(touch)/`event2`(pen) → pointer/touch/scroll events | libevdev/raw evdev | rendering |
| `shell`   | QML chrome: URL bar, nav, scroll, reading mode; wires engine+input+display | display, engine, input | low-level GL/DRM |
| `platform`| Lifecycle: stop/restore xochitl, install layout, OTA re-hook, logging | systemd | UI logic |

**Data flow:** `input` → `shell` → `engine.loadURL/dispatchEvent` → WPE renders → ARGB SHM buffer →
`display` wraps as `QImage` → QtQuick software scene → **epaper QPA** → `imx-drm` → e-ink.

## 5. Roadmap (de-risk via spikes first)

- **Phase 0 — Foundations.** git + structure + docs; reproducible Docker build env with the ferrari
  aarch64 SDK; "hello world" cross-compiles, deploys to `/home`, runs on device.
- **Phase 1 — Display spike.** Minimal Qt6 app draws a test image via `-platform epaper` (xochitl
  stopped); confirm on-screen + clean restore; probe available refresh/waveform modes.
  *Fallback research if epaper QPA is unusable standalone: rmBifrost-style hook or direct-DRM.*
- **Phase 2 — Engine spike.** Cross-build Mesa(llvmpipe) + libsoup3 + WPE(Skia CPU, no media,
  headless/exportable); render a real page to an ARGB buffer → PNG. Proves engine + software GL.
- **Phase 3 — Integration.** Bridge WPE ARGB SHM → `QImage` → epaper. **First real web page on the
  Paper Pro.** Nail pixel format (BGRA), stride, frame sync.
- **Phase 4 — Reading MVP (scope A).** Input (touch/pen → pointer/scroll); shell (URL, back/forward,
  scroll, reading mode: font scaling/contrast); e-ink refresh tuning (fast mono for scroll, full for
  load, partial updates, ghosting control).
- **Phase 5 — Packaging.** Launcher, `/home` install layout, reboot/OTA survival, docs, GitHub.
- **Phase 6 — Toward full browser (scope B).** Tabs, history, bookmarks, on-screen keyboard, forms,
  cookies/TLS, downloads, settings, performance passes. Iterative, post-MVP.

## 6. Risks & mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| epaper QPA not usable by a 3rd-party app standalone | med | Phase-1 spike up front; fallbacks (hook / direct-DRM) researched |
| WPE cross-build complexity (hours, finicky) | high | Containerized SDK, cached builds, slim feature flags |
| Mesa llvmpipe missing surfaceless/extensions WPE probes | med | Verify `webkit://gpu` shows llvmpipe early in Phase 2 |
| e-ink refresh quality (ghosting, latency) on CPU | high | Refresh-mode tuning in Phase 4; (C) direct-DRM as later upgrade |
| Performance of llvmpipe + Skia on A53 | med | Reading content is mostly static; disable animations; partial updates |
| Firmware/OTA breaks install or offsets | med | Install under /home; re-install hook; re-verify device profile |

## 7. Verification

Every phase ends with: **deploy to the real device over SSH and observe**, then a **code-review**
subagent pass and a **simplify** subagent pass. Headless/PNG comparisons for the engine; on-screen
photos/inspection for display; scripted evdev replays for input where feasible.

## 8. Out of scope (now)

Hardware-accelerated WebGL/canvas, video/audio playback, account sync, extensions, multi-window.
Revisit selectively in Phase 6.
