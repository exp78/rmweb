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
**✅ Enum values CONFIRMED** (reverse-engineered from the rMPP toolchain `libqsgepaper.a` by asivery —
https://github.com/asivery/epfb-re , `epframebuffer.h`). This is the authoritative rMPP refresh API:
```cpp
enum EPScreenMode { QualityFastest=0, QualityFast=1, Quality3=3, QualityFull=4, Quality5=5 };  // waveform quality
enum EPContentType { Mono=0, Color=1 };                                                        // mono vs color path
class EPFramebuffer { enum UpdateFlag { NoRefresh=0, CompleteRefresh=1 };                       // CompleteRefresh = FULL flash
  unsigned long swapBuffers(QRect changed, EPContentType, EPScreenMode, QFlags<UpdateFlag>);    // present + refresh
  static EPFramebuffer* createControlledInstance();   // LD_PRELOAD-hook variant; plain instance() also exported
  QImage* getAuxFramebuffer();   // the back buffer you paint INTO
  QImage* getMainFramebuffer(); };
```
Cross-check vs the older rmBifrost `(color, variant, full)` triple (sec. 2): they line up —
`color`=`EPContentType`, `variant`=`EPScreenMode`, `full`=`CompleteRefresh`. So rmBifrost `COLOR_CONTENT→(1,4,0)`
= `swapBuffers(r, Color, QualityFull, NoRefresh)` and `FULL→(1,4,1)` = `swapBuffers(r, Color, QualityFull, CompleteRefresh)`.
`GhostControlMode`/`PixelMode` (from the `nm -D` map above) are NOT in epfb-re yet — still need on-device reversing if used.
**For Phase 4 refresh tuning:** `dlopen("…/libqsgepaper.so") → EPFramebuffer::instance()` (or `createControlledInstance()`)
then `swapBuffers(rect, contentType, screenMode, flags)`. (For the Phase-1 spike none of this is needed — correct Window
sizing alone presents content; the scenegraph auto-refreshes.) **HOW it integrates with our Qt path → see §2a below.**

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
- **asivery `epfb-re`** (https://github.com/asivery/epfb-re) — ★ BEST source for rMPP: the reverse-engineered
  `EPFramebuffer` header (confirmed enums above) + working `swapBuffers` demo + a mode-sweep test. Use THIS.
- **rmBifrost** (rMPP, repo now gone/private; design preserved here): rMPP "waveform" = a `(color, variant, full)`
  triple via `screen_update(QObject* fb, point, point, int color, int variant, int full)` — the same three params
  as epfb's `swapBuffers(rect, EPContentType, EPScreenMode, UpdateFlag)`.
  Enum: `MONOCHROME=0, COLOR_ANIMATION=1, COLOR_FAST=2, COLOR_CONTENT=3, FULL=4` →
  `MONO→(0,0,0)`, `COLOR_FAST→(1,1,0)`, `COLOR_CONTENT→(1,4,0)`, `FULL→(1,4,1)`. (Its binary was fw 3.14/3.15
  only — hardcoded offsets; reuse the design, not the build.)
- For web content (vs paginated books) we must add our own heuristics: scroll vs small-update vs full-repaint
  detection, kill CSS animations (`prefers-reduced-motion` + injected CSS), waveform per update class.
- **Paper Pro color**: ICC profile + palette characterized by wavexx
  (https://www.thregr.org/wavexx/rnd/20260201-remarkable_pro_colors/): auto-contrast + quantized dither to the
  CMY(W) gamut; prefer hardware dither.

### 2a. ACTIONABLE Phase-4 refresh plan for OUR Qt/epaper path (the concrete recommendation)

**Where to hook.** Our display is a `QQuickPaintedItem` (`WpeView`) under the `epaper` QPA, which auto-calls the
scenegraph's `EPFramebuffer::swapBuffers(...)` on every `update()`. That auto path picks a *generic* mode → color
under-develops + ghosts. We take control by calling `swapBuffers` **ourselves** with the right mode per update class.

**Two integration options — try (A) first, fall back to (B):**

- **(A) Bypass the QtQuick scenegraph for the page area; drive `EPFramebuffer` directly.** This is exactly the
  epfb-re recipe and what rmBifrost did. At startup: `dlopen("/usr/lib/plugins/scenegraph/libqsgepaper.so", RTLD_NOW)`
  (Qt loads plugins `RTLD_LOCAL` so resolve via the explicit handle, NOT `RTLD_DEFAULT`), `dlsym` the mangled
  `EPFramebuffer::instance` / `swapBuffers` (or link `-lqsgepaper` and `#include` the epfb-re header). Paint the WPE
  BGRA frame into `instance()->getAuxFramebuffer()` (a full-screen `QImage*`), then call `swapBuffers(dirtyRect,
  content, mode, flags)` with our chosen mode. Keep a tiny QML chrome layer above (toolbar) refreshed the normal way,
  or also draw it into the aux buffer. **This gives per-update waveform control — the thing the plain QPA can't do.**
  ⚠️ epfb-re's `createControlledInstance()` uses LD_PRELOAD QImage-ctor hooks to discover the two internal buffers;
  the simpler plain `instance()` + `getAuxFramebuffer()` is enough if those symbols resolve on our fw. Verify on device.

- **(B) Stay on the QPA, nudge it.** If grabbing the buffer fights QtQuick, keep painting via `WpeView` and only
  call `EPFramebuffer::instance()->swapBuffers(fullScreenRect, Color, QualityFull, CompleteRefresh)` *after* a page
  load / periodically to force the color full-flash, letting the auto path handle fast partials. Coarser, but minimal
  change to the working Phase-3b code. (This is the `setForceFull`-equivalent for the new API.)

**Per-update-class mode table (a reading browser → reuse KOReader/rmBifrost designs):**
| Event | content | mode | flag | rationale |
|---|---|---|---|---|
| Page load / navigation (color) | `Color` | `QualityFull` (4) | `CompleteRefresh` | full multi-pass waveform develops color + clears ghosts (~1 s, matches stock) |
| Scroll tick / panning text | `Mono` (0) | `QualityFastest`/`QualityFast` (0/1) | `NoRefresh` | fast mono partial (~350 ms class) — no flash, slight ghosting OK |
| Small mono UI update (caret, toolbar, link highlight) | `Mono` | `QualityFast` (1) | `NoRefresh` | snappy partial, bounded region |
| Image/color region settled (post-scroll) | `Color` | `Quality3`/`QualityFull` | `NoRefresh` | redo just that region in color once motion stops |
| Anti-ghost flush | `Color` | `QualityFull` | `CompleteRefresh` | the periodic flash (below) |

**Debounce + anti-ghost control loop (steal netsurf + KOReader):**
1. **Coalesce** dirty rects into one bounding box; run an async **debounced redraw on a ~5 Hz / ~200 ms timer**
   (netsurf-reMarkable) so a burst of WPE `buffer-rendered` frames = **one** `swapBuffers` per tick. While scrolling,
   emit `Mono`/`QualityFast`/`NoRefresh` partials.
2. **`FULL_REFRESH_COUNT`** (KOReader): keep a partial counter; **every Nth partial (start N≈8–12, tune on device)
   AND on `scroll-end`/page-load, force `Color`+`QualityFull`+`CompleteRefresh`** over the whole screen to clear
   accumulated ghosting. Reset counter on every full.
3. **8-px align** the dirty rect (round x/y down, w/h up to ×8) before `swapBuffers` (E-Ink controller alignment).
4. **Block animation churn**: inject `* { animation:none!important; transition:none!important; scroll-behavior:auto!important }`
   + emulate `prefers-reduced-motion: reduce` in WPE, so CSS doesn't trigger endless partials.
5. **Throttle waits**: `swapBuffers` returns a marker (the `unsigned long`); only block on completion before the *next
   full*, not on every partial, to keep scrolling fluid (KOReader marker-wait pattern).

**Reference impls to copy from:** epfb-re `test.cpp`/`OLD/modetest.cpp` (exact `swapBuffers` call + a mode-sweep
harness — run it on device to time each `(EPContentType,EPScreenMode,flag)` combo and pick our constants);
netsurf-reMarkable `libnsfb` (dirty-box + debounce thread); KOReader `framebuffer_mxcfb.lua` (`FULL_REFRESH_COUNT`,
per-update waveform, dither, marker waits).

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
- **Input:** Elan (`elants_spi`), NOT Wacom. `event0`=power, `event1`=hall, **`event2`="Elan marker input"
  (PEN), `event3`="Elan touch input" (FINGER TOUCH)** — resolve by NAME via `EVIOCGNAME`, never by `eventN`.
  On aarch64 `struct input_event` is **24 bytes**. Transforms (KOReader): pen `x*1620/11180, y*2160/15340`;
  touch `x*1620/2064, y*2160/2832`; no axis swap, no mirror on stock path (verify top-left tap on device).
  ✅ RESOLVED 2026-06-26 (full plan: **`docs/research/remarkable-touch-input.md`**): the epaper QPA DOES have a
  touch handler but posts `handleTouchEvent(nullptr,…)` → Qt drops it (null window / `topLevelAt` miss), so the
  app receives nothing. **Recommendation: read `/dev/input/event3` directly (Protocol-B evdev) and `EVIOCGRAB`
  it** — the grab also silences the QPA's touch path, which is the likely cause of the **WPE-app segfault on
  touch**. The QPA only probes the grab at startup and releases it (no persistent grab), so a direct reader
  works; without a grab, evdev broadcasts to both readers (kernel `drivers/input/evdev.c`).
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

### WPE WebKit 2.48.5 (Skia CPU, software) — built + headless render PROVEN (2026-06-25)
Recipe: `engine/build-wpe.incontainer.sh` (build) + `engine/render-wpe.incontainer.sh` + `engine/wpe_render.c` (render
proof) + `engine/atomic16.c`, orchestrated by `scripts/build-wpe.sh {deps|build|render|all}`. Output:
`build/stage/usr/lib/libWPEWebKit-2.0.so.1.5.10` (139 MB) + `libWPEPlatform-2.0.so` + `WPE{Web,Network,GPU}Process`.
Render → `build/wpe-render.png`: a real page (blue bar + red/green/yellow boxes + anti-aliased DejaVu text).

**Extra deps WebKit hard-requires (cross-built into `build/stage`, see `build_deps` in `scripts/build-wpe.sh`):**
- **`libatomic.so` shim** — cortex-a53 is ARMv8.0 (no LSE); WebKit/libpas calls `__atomic_{load,store}_16` but the SDK
  gcc ships NO libatomic. A tiny spinlock `engine/atomic16.c` provides the 16-byte ops; link WPE with `-latomic`.
- **`libharfbuzz-icu`** — SDK harfbuzz 8.3.0 was built WITHOUT `--with-icu`; compile the single `src/hb-icu.cc` from the
  matching 8.3.0 tarball + hand-write `harfbuzz-icu.pc`. (`find_package(HarfBuzz REQUIRED COMPONENTS ICU)`.)
- **`libtasn1 4.19`** (autotools), **`libwpe 1.16.2`** (meson native-file), **`libxslt 1.1.39`** (autotools, explicit libxml2 flags).

**Build gotchas (all fixed in the scripts):**
- **`PKG_CONFIG_SYSROOT_DIR=$SR` is MANDATORY** (do NOT unset it): an absolute `-I` ignores `--sysroot`, so without it
  `gio-unix-2.0` resolves to the header-LESS container `/usr/include` → `gio/gfiledescriptorbased.h: No such file`.
- **Configure on a CASE-SENSITIVE FS:** the macOS `/work` bind mount is case-insensitive → `ArgumentCodersGLib.h` vs
  `...Glib.h` collide → `incomplete type IPC::ArgumentCoder<GTlsCertificate>`. Build on a docker **named volume**
  (`rmweb-build:/build`, ext4) — which ALSO makes the ~1.5–2.5 h ninja **resumable** across container/VM restarts. (Watch
  the trap: a `cp -a` of the source must NOT drag a stale `_b` onto the volume, or a resume guard reuses a bad configure.)
- **`g++` must be apt-installed** in the container (debian-slim lacks it) or a host-side C++ compile dies with
  `gcc: cannot execute 'cc1plus'`.
- cmake highlights: `-DPORT=WPE -DUSE_SKIA=ON -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_HEADLESS=ON` + media/webgl/
  sandbox/introspection OFF (full set in the script). Native-build trick (unset `CMAKE_TOOLCHAIN_FILE`, hand cmake the
  bare `aarch64-remarkable-linux-g++` + `--sysroot`) lets WebKit run its generated host tools in the aarch64 container.

**Headless render proof (`engine/render-wpe.incontainer.sh`) — 4 things that bit, all fixed (mostly container-only):**
- **glibc loader mismatch:** WPE spawns Web/GPU/Network subprocesses whose ELF interp is `/lib/ld-linux-aarch64.so.1`
  = container **glibc 2.36**, but they need the **2.39** sysroot libc → `undefined symbol __tunable_is_initialized,
  GLIBC_PRIVATE`. Fix (container ONLY): repoint that symlink to `$SR/lib/ld-linux-aarch64.so.1` (a newer ld runs older
  binaries fine). Side effect: bare container commands run *outside* the scoped `env` may segfault — gate the result
  check with the bash `[ -s ... ]` builtin, not `ls`. **On the DEVICE this whole issue is moot (all 2.39 natively).**
- **baked install prefix:** WPE spawns helpers from the ABSOLUTE `/usr/libexec/wpe-webkit-2.0/` (`WEBKIT_EXEC_PATH` is
  NOT honored in 2.48) → symlink `/usr/{libexec,lib,share}/wpe-webkit-2.0` → `$STAGE/usr/...` (or just install to `/usr` on device).
- **load via `webkit_web_view_load_html()`**, NOT a `data:text/html,` URL — an unescaped `#` (e.g. `#fff`) is parsed as
  the URL fragment and truncates the document → blank render. Capture the buffer AFTER `load-changed == FINISHED`.
- **fonts:** the SDK sysroot has `fonts.conf` but ZERO fonts → set `FONTCONFIG_PATH=$SR/etc/fonts`, `HOME=/tmp`, and drop
  a TTF where it scans (`/usr/share/fonts`, e.g. apt `fonts-dejavu-core`); else text paints blank.
- Software-GL env for the whole run: `GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless` +
  `LIBGL_DRIVERS_PATH=$MESA/usr/lib/dri __EGL_VENDOR_LIBRARY_DIRS=$MESA/usr/share/glvnd/egl_vendor.d`.
- Buffer path: `WPEDisplayHeadless` (surfaceless) → GPUProcess renders + `glReadPixels(BGRA)` → `WPEBufferSHM` →
  `wpe_buffer_import_to_pixels()` (BGRA bytes, no DRM/GBM map needed) → libpng. **This is the seam Phase 3 plugs into the epaper QPA.**

### Device runtime bundle — WPE PROVEN on real hardware (Phase 3a, verified on device 2026-06-25)
The full WPE stack renders the same page **on the actual Paper Pro** (native glibc 2.39, no GPU, software GL) via
`scripts/bundle.sh` (deploy) + `scripts/render-on-device.sh`. Bundle = `/home/root/rmweb` (~171 MB): our built `.so` +
Mesa + the `WPE{Web,GPU,Network}Process` helpers + WebKit resources/injected-bundle + `bin/wpe_render`.
- **Dependency closure must be TRANSITIVE:** depth-1 `NEEDED` misses dlopen'd + nested deps. Walk it with
  `aarch64-remarkable-linux-readelf -d` (the debian container has NO host `readelf`/`objdump` — use the SDK cross one).
  Of 48 sonames, 21 are ours; the device provides all external ones EXCEPT **libsqlite3, libwebp{,demux,mux},
  libsharpyuv** → copy those 5 from the SDK sysroot into the bundle (libsharpyuv is a transitive dep of libwebp 7.1.8).
- **`/` is mounted READ-ONLY** → cannot symlink `/usr/libexec/wpe-webkit-2.0` (the baked prefix WPE spawns helpers from;
  `WEBKIT_EXEC_PATH` is ignored in 2.48, and `/usr/libexec` already holds dbus/sftp/fc-cache so it can't be shadowed
  wholesale). Fix without touching rootfs: **overlay-mount** `/usr/libexec` (lowerdir = real, upperdir = our
  `wpe-webkit-2.0`), `umount` on exit. (Clean fix = rebuild with `-DCMAKE_INSTALL_PREFIX=/home/root/rmweb` in Phase 5.)
- Device env: `LD_LIBRARY_PATH=/home/root/rmweb/lib` + the softpipe GL env + `WEBKIT_INJECTED_BUNDLE_PATH` +
  `FONTCONFIG_PATH=/etc/fonts` (device ships 15 system TTFs — text renders without bundling a font) + `HOME=/home/root`.
- **No glibc loader hacks on device** (everything is 2.39 native) — those were container-only.

### WPE → Qt6 → e-ink integration (Phase 3b, verified on device 2026-06-25)
A Qt6 app (`engine/wpeqt/main.cpp`; build `scripts/build-wpeqt.sh`; run `scripts/run-wpeqt-on-device.sh {save|show}`)
is the WPE UIProcess and shows a **live web page on the Paper Pro e-ink**.
- **WpeEngine on a worker QThread** owns a `GMainContext` (pushed thread-default) + `GMainLoop`. Create the
  context/loop in the CONSTRUCTOR (GUI thread, before `moveToThread`) so `stop()` reads them race-free, then quit via
  `g_main_context_invoke()`. The worker runs `g_main_loop_run` — **NOT a Qt event loop** — so you can't reach it with
  `QMetaObject::invokeMethod(QueuedConnection)`; marshal through GLib.
- **buffer-rendered → QImage:** WPE's BGRA buffer (ARGB8888 little-endian, memory B,G,R,A) maps **directly** to
  `QImage::Format_ARGB32` on little-endian — no channel swap. Deep-copy (`img.copy()`, an argument prvalue) BEFORE
  `g_bytes_unref`; emit `frameReady(QImage)` to the GUI thread (queued — receiver lives there).
- **Display:** a `QQuickPaintedItem` (`WpeView`) paints the frame full-screen; inline QML `Window` sized to
  `Screen.width/height` (the Phase 1 cure), `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`, xochitl stopped. The
  epaper backend refreshes on `update()`. (Color/Gallery-3 full-refresh tuning = Phase 4.)
- **Build gotcha:** WPE pulls in GLib `gio`, whose GDBus structs have members named `signals`/`slots`, colliding with
  Qt's keyword macros → compile with **`QT_NO_KEYWORDS`** and use `Q_SIGNALS`/`Q_SLOTS`/`Q_EMIT`.
- **Cross-build:** seed `build/stage` + `build/stage-mesa` into the sysroot, then the normal OE CMake toolchain finds
  Qt6 (device) + WPE (pkg-config); Qt6 cross moc works out of the box. Device platform plugins: `libqoffscreen.so`
  (save mode) + `libepaper.so` (show); PNG write is built into QtGui (no plugin). BusyBox on device has **no
  `timeout`** → background the app + `sleep` + `kill`. The output PNG path is argv-configurable.
