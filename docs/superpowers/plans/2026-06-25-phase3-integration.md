# Phase 3 — WPE → e-ink integration (web page on the Paper Pro) Implementation Plan

> Spike-style: implement task-by-task, each ends with a concrete on-device verification gate. Long builds run in background.

**Goal:** Show a real web page, rendered by the cross-built WPE WebKit, on the reMarkable Paper Pro e-ink — by feeding
WPE's ARGB buffer into the Qt6 `epaper` QPA (the Phase 1 display path).

**Architecture:** The Qt6 `QGuiApplication` is the WPE **UIProcess**. A dedicated worker thread runs WPE's GLib main
loop with `WPEDisplayHeadless` (surfaceless softpipe EGL, exactly as Phase 2). On `WPEView::buffer-rendered` it copies
the BGRA pixels into a `QImage` and hands it to the GUI thread via a **queued** signal. A full-screen QtQuick item paints
that image; the `epaper` QPA pushes it to e-ink with a **FULL refresh** (color/Gallery-3 content needs full waveforms).
`xochitl` is stopped on entry and restored on exit (Phase 1 lifecycle).

**Tech stack:** Qt 6.8.2 (device), WPE WebKit 2.48.5 + Mesa softpipe (Phase 2, in `build/stage*`), the ferrari SDK.

## Global Constraints
- Install ONLY under **`/home/root/rmweb`** (rootfs `/` is full). Bundle our built `.so` (WPE, Mesa, libsoup, …);
  reuse device libs (Qt, glib, icu, cairo, freetype, harfbuzz, openssl, libcurl, libxml2, png/jpeg, libdrm).
- Software-GL env for WPE + subprocesses: `GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless`
  + `LIBGL_DRIVERS_PATH` / `__EGL_VENDOR_LIBRARY_DIRS` pointing at the bundled Mesa.
- On device glibc is **2.39 natively** → none of the container loader/`PKG_CONFIG_SYSROOT_DIR` hacks are needed at runtime.
- Present via **QtQuick (QML) ONLY** with the Window sized to `Screen.width/height`; `QT_QPA_PLATFORM=epaper
  QT_QUICK_BACKEND=epaper`; **FULL refresh** (`EPFrameBuffer setForceFull` is class `EPFramebuffer` — see research-reuse §1).
- The WPE multi-process helpers are spawned from the **baked** `PKGLIBEXECDIR=/usr/libexec/wpe-webkit-2.0/`
  (`WEBKIT_EXEC_PATH` is ignored in 2.48). On device, provide that path (symlink into `/usr/libexec`, restored on exit;
  a clean `-DCMAKE_INSTALL_PREFIX=/home/root/rmweb` rebuild is deferred to Phase 5 packaging).

---

## Stage 3a — Device runtime bundle + on-device headless render (no Qt yet)
*De-risks the bundle and proves WPE runs on the real hardware before adding Qt integration complexity.*

### Task 3a.1: Compute the runtime `.so` closure
**Gate:** A definitive bundle list. `readelf -d` every NEEDED across `libWPEWebKit`, `libWPEPlatform`, the 3 `WPE*Process`
helpers, and `wpe_render`; split into **bundled** (present in `build/stage` / `build/stage-mesa`) vs **external** (must be
on device). Every external soname is then confirmed present on the device (`ssh … find /usr/lib /lib`).

### Task 3a.2: Assemble + deploy the bundle to `/home/root/rmweb`
**Files:** `scripts/bundle.sh` (host) — copy `build/stage/usr/lib` + `build/stage-mesa/usr/lib` → `rmweb/lib`,
`build/stage/usr/libexec/wpe-webkit-2.0` → `rmweb/libexec/…`, a DejaVu TTF → `rmweb/fonts`, `build/wpe_render` → `rmweb/bin`.
rsync/scp to device. **Gate:** files on device; `du -sh /home/root/rmweb` fits in `/home` (46 GB free).

### Task 3a.3: On-device headless render (the hardware proof)
**Files:** `scripts/render-on-device.sh` — ssh: set `LD_LIBRARY_PATH=/home/root/rmweb/lib`, the softpipe GL env,
`FONTCONFIG_PATH`/font dir, create the `/usr/libexec/wpe-webkit-2.0` symlink (trap-restore), run `rmweb/bin/wpe_render`
writing `/home/root/rmweb/out.png`. scp back. **Gate:** the PNG (bar + boxes + AA text) renders **on the device** — engine
proven on real aarch64 hardware, native glibc, software GL. Commit; record the exact bundle list in `docs/research-reuse.md`.

## Stage 3b — Qt + WPE integration (web page on e-ink)

### Task 3b.1: Qt+WPE skeleton (worker thread + headless view)
**Files:** `engine/wpeqt/` — `WpeEngine` (QObject): owns a `GMainContext`/`GMainLoop` on a `QThread`, creates a
`WebKitWebView` with `WPEDisplayHeadless`, loads a URL, emits `frameReady(QImage)` (deep-copied BGRA→`QImage::Format_RGBA8888`)
from `buffer-rendered`, marshalled to the GUI thread. **Gate (device):** `frameReady` fires on device with the right size.

### Task 3b.2: Paint the frame in QtQuick
**Files:** `engine/wpeqt/WpeView` (a `QQuickItem` exposing the latest `QImage` via a scene-graph texture node, or a
`QQuickImageProvider` + bumped `Image`). QML `Window` sized to `Screen.width/height`, the item `anchors.fill: parent`.
**Gate:** off-device sanity (or directly device) shows the rendered frame in the item.

### Task 3b.3: On-device web page on e-ink
**Files:** `scripts/run-rmweb-on-device.sh` (extends `run-on-device.sh`: bundle env + helper symlink + stop/restore
xochitl + `epaper` QPA + FULL refresh). **Gate:** a recognizable web page (start with the Phase 2 test HTML, then a real
URL like a simple article) appears on the Paper Pro e-ink. Iterate refresh quality (full vs partial) per research-reuse §2.

### Task 3b.4: Review + simplify + docs + commit
code-review subagent + simplify subagent on the new `engine/wpeqt/` + scripts; update `docs/research-reuse.md` and `CLAUDE.md`.

## Verification
3a.3 = headless PNG on device. 3b.3 = a web page visible on e-ink. Each task verifies on the real device before moving on.

## Self-review
- Covers the spec's data flow (input→shell→engine→ARGB SHM→display→e-ink) for the engine→display seam. ✓
- De-risks bundle (3a) before integration (3b) — matches do-and-verify. ✓
- Threading: WPE GLib loop on a worker thread avoids fighting Qt's event loop; frames cross via queued `QImage` copy. ✓
- Known risk: the baked `/usr/libexec` helper path on a full rootfs — spike uses a restored symlink; clean prefix rebuild deferred to Phase 5.
