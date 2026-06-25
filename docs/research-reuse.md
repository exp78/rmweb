# Research & Reuse map — don't redo solved work

Synthesized from a web research sweep on 2026-06-24 (primary sources cited). The goal: **reuse
existing community/official work** wherever possible and only build what's genuinely missing.
Treat "rMPP" = reMarkable Paper Pro ("ferrari", aarch64, color E Ink Gallery 3 / ACeP2).

## 0. The single biggest fact

**rMPP is a different world from rM1/rM2.** It drives the panel through **DRM/KMS (`/dev/dri/card0`)** —
there is **no `/dev/fb0`, no `mxc_epdc`, no `MXCFB_SEND_UPDATE` ioctl**. So rm2fb, libremarkable, rmkit,
reStream, xugro/rmpp-framebuffer, etc. **do not apply**. Present through reMarkable's **Qt epaper stack**
(or reverse-engineer DRM yourself — undocumented). Also: **xochitl unlocks the LUKS `/home` at boot**, so
always let it start, then `systemctl stop xochitl`.

## 1. How to actually draw to the Paper Pro screen (our Strategy A — chosen)

- **QtQuick (QML) only.** A `QRasterWindow`/`QWidget`/`QPainter` window bypasses `libqsgepaper` and never
  refreshes the panel (matches our first failed attempt). QtWidgets isn't even on the device.
- Platform plugin `/usr/lib/plugins/platforms/libepaper.so`; scenegraph adaptation
  `/usr/lib/plugins/scenegraph/libqsgepaper.so` (class `QsgEpaperPlugin`, key `epaper`).
- Run: `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper ./app -platform epaper`, with **xochitl stopped**.
- **Size the Window to `Screen.width`/`Screen.height` in QML** (official recipe). Forcing geometry from C++
  after creation gives a 0×0 first frame → only a partial update → white screen with a content fragment
  (exactly our bug).
- The scenegraph **auto-calls `EPFrameBuffer::sendUpdate(...)`** per dirty region — apps normally don't call
  it. The official `hello_remarkable` has zero refresh code.
- **Color content needs a FULL refresh.** Gallery 3/ACeP2 develops color only on the full multi-pass
  waveform; partial/fast (DU/Mono) updates can be invisible → white. Lever: `EPFrameBuffer::setForceFull(true)`
  + dirty the scene once after show. (We resolve `setForceFull` by symbol via `dlsym`, since libqsgepaper is
  already loaded; mangled name `_ZN12EPFrameBuffer12setForceFullEb`.)

### EPFrameBuffer API (header: Eeems-Org `remarkable-template-qt-app/src/vendor/epaper/epframebuffer.h`)
```cpp
static EPFrameBuffer* instance();
static QImage* framebuffer();                 // the shared QImage the scenegraph renders into
enum WaveformMode { Initialize, Mono /*DU*/, Grayscale /*GL16*/, HighQualityGrayscale /*GC16*/, Highlight };
enum UpdateMode   { PartialUpdate = 0x0, FullUpdate = 0x1 };
Q_INVOKABLE static void setForceFull(bool);   // every update becomes full-screen — the key knob
public slots:
  static void clearScreen();
  static void sendUpdate(QRect rect, WaveformMode waveform, UpdateMode mode, bool sync = false);
  static void waitForLastUpdate();
```
Under the hood it's the Kindle-style `MXCFB_SEND_UPDATE`-equivalent. **rMPP's color `WaveformMode` enum ints
differ from the mono rM2 table — rely on the NAMES, not raw integers.** Source `epframebuffer_acep2.cpp` is
not public.

### ⚠️ rMPP CORRECTION — verified on device (`nm -D` on the real `libqsgepaper.so`, 2026-06-25)
The header above is the **rM1/rM2 (Qt5) `EPFrameBuffer`**. The Paper Pro uses a **different, newer class —
note the lowercase 'b': `EPFramebuffer`** — and a different refresh API. These symbols are **exported** in
`/usr/lib/plugins/scenegraph/libqsgepaper.so` (callable via `dlopen(plugin) + dlsym`; Qt loads plugins
`RTLD_LOCAL`, so `RTLD_DEFAULT` does NOT see them — dlopen the .so explicitly):
```
EPFramebuffer* EPFramebuffer::instance();                       // _ZN13EPFramebuffer8instanceEv
void EPFramebuffer::forceInstance(EPFramebuffer*);
void EPFramebuffer::swapBuffers(QRect, EPContentType, EPScreenMode, QFlags<UpdateFlag>);   // the present/refresh call
void EPFramebuffer::swapBuffers(QRegion, EPContentMap, EPScreenModeMap, QFlags<UpdateFlag>); // multi-region
void EPFramebuffer::ghostControl(GhostControlMode);             // ghosting control — for Phase 4 anti-ghost
void EPFramebuffer::setBuffers(std::tuple<QImage,QImage>);
// impls: EPFramebufferAcep2 (color) :: swapBuffers_impl(...), ghostControl(...);
//        EPFramebufferSwtcon :: update(QRect, int, PixelMode, int)
```
Enums to reverse for exact values: `EPContentType`, `EPScreenMode`, `UpdateFlag`, `GhostControlMode`, `PixelMode`.
**For Phase 4 refresh tuning:** `dlopen("/usr/lib/plugins/scenegraph/libqsgepaper.so") → EPFramebuffer::instance()`
then `swapBuffers(rect, contentType, screenMode, flags)` for full vs partial / waveform, and `ghostControl(...)`
for ghost clearing. (For the basic Phase-1 spike none of this is needed — correct Window sizing alone presents
content; the scenegraph auto-refreshes.)

### Official references (copy these)
- reMarkable Qt epaper guide: https://developer.remarkable.com/documentation/qt_epaper
- Official examples (run on rMPP): https://github.com/reMarkable/remarkable-developer-examples (`hello_remarkable`, `calculator`)
- QPA repos (platform side): https://github.com/reMarkable/qt5-qpa-epaper , https://github.com/reMarkable/epaper-qpa
- EPFrameBuffer header + symbol map: https://github.com/Eeems-Org/remarkable-template-qt-app ,
  https://github.com/canselcik/libremarkable/blob/master/reference-material/libqsgepaper.md ,
  https://github.com/pl-semiotics/libqsgepaper-snoop

## 2. Refresh strategy to STEAL (our Phase 4)

The Qt path auto-refreshes but ghosts/leaves color un-developed. Reuse these *designs* (reimplement in C++):
- **netsurf-reMarkable / libnsfb-reMarkable** (https://github.com/alex0809/libnsfb-reMarkable): accumulate a
  **dirty bounding box** (MIN/MAX over rects), a **debounced async redraw thread (~200 ms / ~5 Hz)** that
  coalesces to one update per tick, **partial-by-default** waveform. Simple; ghosts over time.
- **KOReader `framebuffer_mxcfb.lua`** (https://github.com/koreader/koreader-base/blob/master/ffi/framebuffer_mxcfb.lua):
  **`FULL_REFRESH_COUNT`** — promote to a full (flashing) refresh every N partials to clear ghosting;
  per-update **waveform selection** (fast A2/DU for text/scroll, GC16 for images/final); hardware **dither**
  with **8-px alignment** (round x/y down, w/h up to ×8); **marker waits** for completion.
- **rmBifrost** (rMPP, abandoned but the map — https://github.com/TiagoJMartins/rmBifrost): rMPP "waveform" =
  a `(color, variant, full)` triple via `screen_update(QObject* fb, point, point, int color, int variant, int full)`.
  Enum: `MONOCHROME=0, COLOR_ANIMATION=1, COLOR_FAST=2, COLOR_CONTENT=3, FULL=4` →
  `MONO→(0,0,0)`, `COLOR_FAST→(1,1,0)`, `COLOR_CONTENT→(1,4,0)`, `FULL→(1,4,1)`. (Its binary is fw 3.14/3.15
  only — hardcoded offsets; reuse the design, not the build.)
- For web content (vs paginated books) we must add our own heuristics: scroll vs small-update vs full-repaint
  detection, kill CSS animations (`prefers-reduced-motion` + injected CSS), waveform per update class.
- **Paper Pro color**: ICC profile + palette characterized by wavexx
  (https://www.thregr.org/wavexx/rnd/20260201-remarkable_pro_colors/): auto-contrast + quantized dither to the
  CMY(W) gamut; prefer hardware dither.

## 3. WPE WebKit build — REUSE (our Phase 2)

- **WPE defaults to Skia CPU rendering** on embedded since 2.46 (Igalia found it faster than GPU there). Env:
  `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`, `WEBKIT_SKIA_CPU_PAINTING_THREADS=N`, `WEBKIT_SKIA_GPU_PAINTING_THREADS=0`.
  Skia paints on CPU into buffers itself — we still need an **EGL for the compositor** → **Mesa surfaceless
  llvmpipe** (`EGL_PLATFORM=surfaceless`, `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`).
- **Igalia `meta-webkit` (scarthgap branch)** — bitbake recipes for `wpewebkit` (~2.52.x), `libwpe`,
  `wpebackend-fdo`, `cog`: a near-exact dependency/flag manifest for our scarthgap/glibc-2.39 cross build,
  even if we don't use full Yocto. https://github.com/Igalia/meta-webkit
- **Buffer export**: prototype with **WPEBackend-fdo exportable SHM** (more examples;
  `EGL_PLATFORM_SURFACELESS_MESA`), target **WPEPlatform headless** (modern, WPE 2.46+, DMA-BUF/SHM via
  `buffers-changed`; see MiniBrowser `--headless`). https://github.com/Igalia/WPEBackend-fdo
- Build deps reference (x86_64): `Igalia/webkit-container-sdk`. **No prebuilt WPE-for-reMarkable exists** —
  build from source with our ferrari SDK Docker. Slim flags: `USE_GSTREAMER=OFF`, `ENABLE_WEBGL=OFF`, etc.
  Must-have deps to bundle: libsoup3 (+sqlite3/libpsl/nghttp2), libwebp, libxkbcommon, libepoxy, Mesa, gnutls.

## 4. Lifecycle / system facts (our Phase 5)

- **Boot order:** let xochitl start (unlocks LUKS `/home`) → `systemctl stop xochitl` → run app →
  `systemctl start xochitl`. `rm-sync.service` follows xochitl. Never disable xochitl pre-unlock.
- **Persistence:** `/` is read-only ext4 A/B (`swupdate`); `/etc`,`/var/*` are tmpfs overlays (reset on
  reboot); **only `/home` (LUKS ~48 GB) survives reboot AND OTA** → install under `/home/root/rmweb`.
  Block OTA at the network layer (the Settings toggle re-enables itself).
- **PIN/lock** lives inside xochitl; with xochitl stopped + our app foreground the PIN doesn't appear.
  Resume-from-sleep lock with a custom app = **fragile/undocumented** — test on device.
- **Suspend:** foreground app owns the idle timer (with xochitl stopped the device won't auto-sleep). To
  sleep: draw a pre-suspend image → `systemctl suspend` → set RTC `wakealarm`. Suspend-notify driver:
  write our PID to a sysfs `target_pid`, receive `SIGRTMAX-1` (prepare) / `SIGRTMAX` (post). Expect a full
  repaint on resume (FPGA bridge reloads).
- **Input:** Elan (`elants_spi`), NOT Wacom. `event0`=power, `event1`=hall, `event2`="Elan marker input"
  (pen), `event3`="Elan touch input", `event4`=Type Folio keyboard. Open **shared, not `EVIOCGRAB`**. On
  aarch64 `struct input_event` is **24 bytes**. Transforms (KOReader): pen `x*1620/11180, y*2160/15340`;
  touch `x*1620/2064, y*2160/2832`; no axis swap. ⚠️ Our earlier on-device recon showed `touchscreen0 -> event2`
  and the epaper QPA already feeds touch via `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` — **verify the exact
  event→device mapping on the device before wiring input.** The epaper QPA may deliver pen/touch to Qt for us.
- **Packaging:** **Vellum** (apk; replaced Toltec for rMPP, model/fw-aware), `rmpp-entware`, and **XOVI +
  rm-appload** (in-xochitl launcher; our Strategy-B alternative). Frontlight
  `/sys/class/backlight/rm_frontlight/brightness` (0–2047). Battery sysfs `max1726x`.

## 5. Strategy B (alternative, for later) — inject into xochitl

XOVI (https://github.com/asivery/xovi, LD_PRELOAD function-hooking, aarch64-first) + qt-resource-rebuilder +
**rm-appload** (https://github.com/asivery/rm-appload, in-UI app launcher + QML/native SDK, handles rMPP
aspect ratios). Active (2026). This is how the community ships apps (KOReader, terminal) **without replacing
xochitl**. Consider for Phase 5 distribution if standalone (Strategy A) proves awkward. Read-side reference:
goMarkableStream (https://github.com/owulveryck/goMarkableStream) reads xochitl's framebuffer from
`/proc/<pid>/mem` (BGRA, geometry 1632×2154) — confirmed working on rMPP.

## 6. Do NOT reuse on rMPP (waste of time)

Toltec (armv7, superseded by Vellum) · rm2fb · xugro/rmpp-framebuffer (dead PoC, clears but can't draw) ·
rmkit/remux/harmony/nao/genie (need `/dev/fb0` + rm2fb, EVIOCGRAB) · libremarkable (rM2 dims + MXCFB) ·
Oxide/draft · reStream/reSnap/rmview. All assume rM1/rM2 `/dev/fb0` + mxcfb.

## 7. Key links (quick index)

- Official: developer.remarkable.com/documentation/{qt_epaper,sdk,xochitl} · reMarkable/remarkable-developer-examples
- Kernel (ground truth): https://github.com/reMarkable/linux-imx-rm (branch `rmpp_6.12.49_v3.27.x`)
- Display reuse: alex0809/netsurf-reMarkable · koreader/koreader-base · TiagoJMartins/rmBifrost · canselcik/libremarkable (wiki)
- WPE: Igalia/meta-webkit · Igalia/WPEBackend-fdo · wpewebkit.org · docs.webkit.org Graphics · docs.mesa3d.org/drivers/llvmpipe
- Ecosystem: asivery/xovi · asivery/rm-appload · owulveryck/goMarkableStream · vellum-dev/vellum · hmenzagh/rmpp-entware · reHackable/awesome-reMarkable

## 8. Phase 2 build facts (verified 2026-06-25)

**Software GL — Mesa 24.0.9 (softpipe, NO LLVM):** builds + the EGL surfaceless smoke test passes →
`GL_RENDERER=softpipe`, `OpenGL ES 3.1 Mesa 24.0.9`. Artifacts: `libEGL.so / libGLESv2.so / libgbm.so` (aarch64).
- **Runtime env (use these to run anything GL, incl. WPE):** `GALLIUM_DRIVER=softpipe` (NOT `swrast` — that's the
  DRI module name and makes `eglInitialize` fail), `LIBGL_ALWAYS_SOFTWARE=1`, `EGL_PLATFORM=surfaceless`.
- **Build approach (key trick, reuse for WPE):** the `rmweb-sdk` container is **aarch64**, and the SDK cross-gcc
  produces binaries that **run natively in the container**. The OE meson-wrapper's cross-file sets
  `needs_exe_wrapper=true` + a broken x86-64 native toolchain → breaks any build that runs generated host tools
  (Mesa, and very likely WebKit). Fix: do a **native aarch64 build** with a custom meson native-file (or, for CMake,
  a native config) pointing the SDK gcc at the device sysroot, bypassing the OE cross-wrapper. (Plain meson via the
  OE wrapper still works for simple libs like libxkbcommon that don't run generated tools.)
- Mesa meson: `-Dgallium-drivers=swrast -Dllvm=disabled -Degl-native-platform=surfaceless -Dplatforms=
  -Dgles2=enabled -Dgbm=enabled -Dglx=disabled -Dvulkan-drivers=` (in 24.0.9 `surfaceless` moved from `platforms`
  to `egl-native-platform`). Python build deps (mako/markupsafe/packaging) via `python3 -m ensurepip` → `build/pydeps`
  (set `SSL_CERT_DIR=$OECORE_NATIVE_SYSROOT/etc/ssl/certs/`, `PYTHONPATH=build/pydeps`).
- On the **device** these LD/loader gymnastics are moot — the device's own glibc loads everything normally.

**Built deps (all into `build/stage/usr`, aarch64), exact working configs:**
- `libxkbcommon 1.7.0` (meson) — `-Denable-wayland=false -Denable-x11=false -Denable-docs=false -Denable-tools=false`.
- `libpsl 0.21.5` (meson) — `-Druntime=no -Dbuiltin=true -Dtests=false` (NB: `builtin` is a **boolean** in 0.21.5;
  `-Dbuiltin=no` errors — use `true`, which embeds the PSL data so no on-device PSL file is needed).
- `libepoxy 1.5.10` (meson) — from the **GNOME mirror** `download.gnome.org/sources/libepoxy/1.5/` (the GitHub
  release tarball 404s). `-Dglx=no -Dx11=false -Degl=yes -Dtests=false -Dc_args=-I/work/build/stage-mesa/usr/include`
  (needs EGL/KHR headers at BUILD time — they live in `build/stage-mesa`; libepoxy `dlopen`s libEGL at runtime, links only `libdl`).
- `libsoup 3.6.0` (meson) — `-Dvapi=disabled -Dintrospection=disabled -Dtests=false -Dsysprof=disabled -Ddocs=disabled
  -Dpkcs11_tests=disabled -Dautobahn=disabled -Dtls_check=false`. HTTPS needs **glib-networking** at runtime (deferred;
  http/file/data work without it).
- ⚠️ **The SDK ships NO glib codegen tools** (`glib-mkenums`, `glib-genmarshal`). They were extracted from glib
  2.78.6 source and staged at **`build/stage/usr/bin`** — **prepend that to PATH for any glib-based build (libsoup,
  and WebKit itself needs them too)**.
