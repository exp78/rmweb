# Phase 2 — WPE WebKit engine (software) Implementation Plan

> Implement task-by-task; each task ends with a concrete verification gate. Long builds run in the background.

**Goal:** Cross-build the missing WebKit dependencies + **WPE WebKit (software / Skia CPU)** for the Paper Pro,
and **headless-render a real web page to a PNG** — proving the engine + software-GL stack on aarch64 with no GPU driver (the SoC has a GPU on die, but the stock OS ships no driver, so CPU-only in practice).

**Architecture:** Build in the `rmweb-sdk` container (meson auto-applies the OE aarch64 cross-file). Missing deps
+ WPE install into a **persistent staging prefix** `build/stage/usr` (host-mounted, gitignored). Each build step
seeds the SDK sysroot from `build/stage` first, so later deps/WebKit find earlier ones. The same `build/stage/usr/lib`
becomes the device bundle (`/home/root/rmweb/lib`) later.

**Tech stack:** meson+ninja / cmake, the ferrari SDK (GCC 13.4, glibc 2.39, cortex-a53). Mesa **swrast** software
EGL/GLES (NO LLVM — llvmpipe needs LLVM which isn't in the SDK; WPE uses GL only for compositing, page raster is Skia CPU).

## Global Constraints
- No GPU driver on device (GPU on die, unused in the stock OS) → Mesa **swrast** (gallium `swrast`, `egl`, `gbm`, `platforms=surfaceless`; no vulkan/glx/llvm).
- WPE built software: Skia CPU is the WPE default; disable media (`USE_GSTREAMER=OFF`), WebGL, etc.
- Install everything to `build/stage/usr`; bundle the runtime `.so`s under `/home/root/rmweb/lib` (rpath/$ORIGIN).
- Reuse SDK-provided deps (glib, icu, cairo, freetype, harfbuzz, fontconfig, libxml2, png, jpeg, openssl, zlib,
  **sqlite3, libwebp, libnghttp2**, lcms2, libdrm) — do NOT rebuild them.

## Already present in SDK sysroot (verified) — do not build
glib-2.0/gio, icu, harfbuzz, freetype2, fontconfig, cairo, libxml-2.0, libpng, libjpeg/turbojpeg, openssl, zlib,
**sqlite3, libwebp(+demux/mux), libnghttp2**, lcms2, libdrm.

## Must build (missing)
`libpsl` → `libxkbcommon` → `libepoxy` → `libsoup-3.0` → **Mesa (swrast EGL/GLES/gbm/surfaceless)** →
**WPE WebKit**. (HTTPS later: `glib-networking` with the openssl backend.)

---

### Task 1: Staging prefix + `scripts/build-dep.sh`
**Files:** Create `scripts/build-dep.sh` (fetch + cross-build a meson/cmake/autotools project; seed sysroot from
`build/stage`; install into `build/stage`). Add `build/` already gitignored.
- [ ] Verify by building **libxkbcommon** (meson, `-Denable-wayland=false -Denable-x11=false -Denable-docs=false`):
  produces `build/stage/usr/lib/libxkbcommon.so` and `.../pkgconfig/xkbcommon.pc`. Gate: both files exist.

### Task 2: small deps
- [ ] `libpsl` (meson, `-Druntime=libidn2` or `-Druntime=no` if idn2 absent). Gate: `libpsl.so` + `.pc`.
- [ ] `libepoxy` (meson, `-Dglx=no -Dx11=false -Degl=yes`). Gate: `libepoxy.so` + `epoxy.pc`.
- [ ] `libsoup-3.0` (meson, `-Dvapi=disabled -Dintrospection=disabled -Dtests=false -Dsysprof=disabled`,
  needs glib✓ sqlite✓ nghttp2✓ libpsl↑). Gate: `libsoup-3.0.so` + `libsoup-3.0.pc`.

### Task 3: Mesa (software EGL/GLES) — the GL piece
- [ ] Build Mesa (meson) with: `-Dgallium-drivers=swrast -Dvulkan-drivers= -Dplatforms=surfaceless
  -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles2=enabled -Dopengl=true -Dllvm=disabled -Dshared-glapi=enabled
  -Ddri3=disabled -Dgallium-vdpau=disabled -Dgallium-va=disabled` (adjust to Mesa version). Gate: `libEGL.so`,
  `libGLESv2.so`, `libgbm.so` in `build/stage/usr/lib`.
- [ ] **EGL smoke test:** a tiny C program (`eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)` →
  `eglInitialize` → `eglGetConfigs` → create a pbuffer/surfaceless context → `glGetString(GL_RENDERER)`), built
  with the SDK + stage, run in the container with `EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
  GALLIUM_DRIVER=swrast`. Gate: prints a software renderer string (e.g. "softpipe"/"llvmpipe"/"swrast"). This
  de-risks the WPE compositor before the multi-hour WebKit build.

### Task 4: WPE WebKit (software) — the long build
- [ ] Fetch a pinned WPE WebKit release (≈2.46–2.48; verify deps against our versions). Configure (cmake):
  `-DPORT=WPE -DCMAKE_BUILD_TYPE=Release -DUSE_GSTREAMER=OFF -DENABLE_WEBGL=OFF -DENABLE_WEBXR=OFF
  -DENABLE_WEB_AUDIO=OFF -DENABLE_VIDEO=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=OFF
  -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_DOCUMENTATION=OFF -DUSE_AVIF=OFF
  -DUSE_JPEGXL=OFF -DENABLE_WPE_PLATFORM=ON` (+ exportable/headless backend; refine at the pinned tag).
  Build in the background (`-j$(nproc)`; can be 1–3 h). Gate: `libWPEWebKit-*.so` + a MiniBrowser/headless tool.
  *Risk:* exact flag set varies by tag — iterate against the configure errors; keep a `scripts/build-wpe.sh`.

### Task 5: headless render → PNG (the proof)
- [ ] Run WPE headless (MiniBrowser `--headless`, or a small program using WPEPlatform headless / WPEBackend-fdo
  exportable SHM) to load a simple page and dump the rendered ARGB buffer to a PNG, with the Mesa software EGL env.
  Gate: a PNG that visibly shows the rendered page (in the container; optionally also run on the device).
- [ ] Commit; record exact versions/flags in a `docs/` note; bundle runtime `.so`s list for Phase 3/5.

## Verification
Each dep: `.so` + `.pc` produced (and `pkg-config --exists` passes for the next build). Mesa: EGL smoke test prints
a software renderer. WPE: links + headless render produces a correct PNG. Then code-review + simplify the scripts.

## Self-review
- Covers spec Phase 2 (cross-build WPE + Mesa, headless render to PNG). ✓
- Dep list verified against the SDK sysroot (build only what's missing). ✓
- Mesa swrast (no LLVM) is the pragmatic choice given no llvm-config in the SDK; EGL smoke test de-risks it before WebKit. ✓
- Exact Mesa/WPE flags will be refined against the pinned versions during execution (spike) — not placeholders, but expect iteration.
