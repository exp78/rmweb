# Driving PROMPT, fully-developed e-ink refreshes for a Qt6 QtQuick app on the reMarkable Paper Pro

**Scope:** rMPP ("ferrari", aarch64, color E Ink **Gallery 3 / ACeP2**, 1620×2160, **no GPU**) running a custom
Qt6 **QtQuick** app under the reMarkable **`epaper` QPA** + the QtQuick **`epaper` scenegraph backend**
(`/usr/lib/plugins/scenegraph/libqsgepaper.so`), with **xochitl stopped**.

**The problem (hard evidence, on-device):** a `QQuickPaintedItem` (`WpeView`) is fed WPE-rendered web frames
(`setImage(QImage)` → `update()`). The WPE frame is produced in ~23 ms (logged flip-latency), but the **physical
panel only visibly updates roughly every ~6 SECONDS**, not per frame. There is also a ~6 s **periodic** WPE
re-render even with no input. So new content reaches the QtQuick scene fast, yet the panel refresh is
slow/throttled, or uses a partial waveform that never develops the grayscale-text-on-white content until some
periodic full refresh.

Synthesized 2026-06-26 from three parallel sourced research sweeps. Tags: **[VS]** = verified-source (URL given),
**[INF]** = inference (reasoned, but not directly attested). This builds on `docs/research-reuse.md` §2/§2a — see
the "Deltas to fold back" section at the end for corrections to that doc.

---

## 0. Executive summary (TL;DR)

1. **Why ~6 s now:** the `epaper` QtQuick backend installs its **own custom `QSGRenderLoop` (`EPRenderLoop`)
   with a custom `QAnimationDriver`** (it exports `EPContext::createAnimationDriver` + `EPRenderLoop::animationDriver`,
   whereas stock Qt's basic loop hard-codes `animationDriver()==nullptr`). On a software backend with **no real
   vsync**, panel *presentation* is gated to that driver's coarse cadence and/or to an internal time/counter-based
   **deferred full refresh** — while your `update()` only does sync+render into the off-screen framebuffer (the
   23 ms you log). **Compounding it:** the auto-path uses a generic/partial waveform, and on Gallery 3
   **grayscale-on-white pushed with a fast/partial waveform does not develop until a full (GC16-class) pass** — so
   even when a partial fires, you may see nothing until the periodic full. Net: scene fast, panel ~6 s.
   *No published source contains the literal "6 s" constant — the number itself is unattributed* [INF, strong].
2. **The per-frame fix (recommended recipe):** take **explicit control of the refresh** via the lowercase-'b'
   `EPFramebuffer::swapBuffers(QRect, EPContentType, EPScreenMode, QFlags<UpdateFlag>)` exported by
   `libqsgepaper.so`. After your item repaints, drive a **debounced** `swapBuffers` with a **per-update-class
   waveform** (fast mono partial per page-turn; full color flash on navigation + every Nth turn).
3. **Which signal / thread:** call from **`QQuickWindow::afterRendering`** (or `frameSwapped` for
   "previous frame is on the panel"), **Direct-connected**. Force `QSG_RENDER_LOOP=basic` so these fire on the
   **GUI thread** — the thread `EPFramebuffer` expects ("wants to run in QT context").
4. **Self-driving vs scenegraph — SAFETY:** **do NOT let both the scenegraph and your code present the same
   pixels.** That is the documented "the magic disappears when custom Qt apps draw on the screen" failure (double
   present / waveform fighting / possible `abort()`). **Pick one owner per pixel region.** Two safe shapes:
   **(A)** keep the scenegraph presenting and only **add** explicit full flashes (minimal change, coarse), or
   **(B)** **take over presentation** of the page area (paint the WPE frame into `getAuxFramebuffer()` and call
   `swapBuffers` yourself; keep QML chrome in a non-overlapping rect). Ship (A), graduate the page area to (B).
5. **Enums for the two key cases:** fast page-turn (grayscale) = `swapBuffers(rect, Mono=0, QualityFast=1,
   NoRefresh=0)`; full anti-ghost / color-develop flash = `swapBuffers(fullRect, Color=1, QualityFull=4,
   CompleteRefresh=1)`. Full-flash every **N=6** partials (KOReader's verified default) **and** on every
   navigation.
6. **Custom-app feasibility:** a standalone Qt app **can** own the panel once xochitl is stopped — `libqsgepaper`
   talks to `/dev/dri/card0` (DRM dumb buffers + atomic KMS; **no `/dev/fb0`**) and arbitrates ownership with a
   **lockfile `/tmp/epframebuffer.lock`** (no DRM leases). **Never run alongside xochitl.** Your Phase-1/3 success
   *is* the standalone proof; the public ecosystem otherwise injects into xochitl rather than running standalone.

---

## 1. WHY the panel updates only ~every 6 s while the QtQuick scene updates fast

### 1.1 The component that actually drives the panel is the closed `libqsgepaper`, not the open QPA

The open-source `reMarkable/qt5-qpa-epaper` and `reMarkable/epaper-qpa` repos do **nothing** to the panel — both
backing-store `flush()`es are no-ops:
- `qt5-qpa-epaper` `QMinimalBackingStore::flush()` `Q_UNUSED`s all args and (in debug) only writes a PNG — no
  `sendUpdate`, no waveform, no timer. [VS] https://raw.githubusercontent.com/reMarkable/qt5-qpa-epaper/master/qminimalbackingstore.cpp
- `epaper-qpa` `EpaperBackingStore::flush()` is an empty no-op. [VS] https://github.com/reMarkable/epaper-qpa/blob/new_devices/epaperbackingstore.cpp

This is **why QtQuick reaches the panel but QtWidgets/`QRasterWindow` never does** (matches `research-reuse.md`):
the real e-ink push lives only in the **closed QtQuick scenegraph adaptation `libqsgepaper`**. So all refresh
timing logic is inside `libqsgepaper`, and the QPA repos are a red herring for this question.

### 1.2 `libqsgepaper` ships a CUSTOM render loop + a CUSTOM animation driver — this is the prime throttle

The exported symbol set of `libqsgepaper` mirrors Qt's upstream basic render loop (`QSGGuiThreadRenderLoop`)
method-for-method: `EPRenderLoop::{update, maybeUpdate, handleUpdateRequest, exposureChanged, show, hide, grab,
releaseResources, createRenderContext}` + `EPContext`. [VS]
https://raw.githubusercontent.com/canselcik/libremarkable/master/reference-material/libqsgepaper.md
Upstream that loop is the single-threaded, `requestUpdate`-driven "basic" loop. [VS]
https://code.qt.io/cgit/qt/qtdeclarative.git/tree/src/quick/scenegraph/qsgrenderloop.cpp

**The decisive difference:** stock Qt's basic loop hard-codes `QAnimationDriver *animationDriver() const { return
nullptr; }`, but `libqsgepaper` **exports both `EPRenderLoop::animationDriver()` and
`EPContext::createAnimationDriver(QObject*)`** — i.e. reMarkable installs its **own custom `QAnimationDriver`**.
[VS] symbol dump above; [INF, strong] you don't ship a `createAnimationDriver` factory to return `nullptr`.

A custom `QAnimationDriver` that is **not vsync-synced** (there is no real vsync on the software/llvmpipe path) and
advances the render/present loop on a **coarse cadence** produces exactly your symptom: the QtQuick **scene**
updates instantly on your `update()` (sync+render into the off-screen buffer = the 23 ms you log), but the
**panel push** — especially any full/GC16 pass — is gated to the driver's cadence, and **fires even with no input**
(an animation tick / periodic ghost-clear with no user action). This is the only single hypothesis consistent with
*both* "scene fast" *and* "panel slow + periodic with no input." [INF, strong]

The `EPFrameBuffer` class also demonstrably carries the **state you'd build a deferred/periodic refresh from** —
`QElapsedTimer m_lastUpdateTimer`, `qint64 timeSinceLastUpdate()`, `int m_lastUpdateId`, and a static per-call
`sendUpdate(...)::updateCounter`. [VS]
https://raw.githubusercontent.com/Eeems-Org/remarkable-template-qt-app/main/src/vendor/epaper/epframebuffer.h
(NB: that header is the **rM1/rM2** capital-'B' `EPFrameBuffer`; whether the rMPP lowercase-'b' `EPFramebuffer`
keeps the same timer/counter is unproven — but it gives clear motive + means for an internal periodic full.)
The `.cpp` bodies are not published anywhere, so the *exact* logic is inference, not proof.

### 1.3 Grayscale-on-white under a fast/partial waveform does NOT develop until a full pass (the "invisible" half)

This is the second, independent half of the problem (the "panel looks unchanged" half, distinct from the cadence
half), and it is well-supported:
- The modern rMPP refresh call is `EPFramebuffer::swapBuffers(QRect changed, EPContentType, EPScreenMode,
  QFlags<UpdateFlag>)`; **`UpdateFlag::CompleteRefresh` (=1) is the bit that forces a full-screen waveform** —
  without it you get a partial that may under-develop. [VS]
  https://raw.githubusercontent.com/asivery/epfb-re/master/epframebuffer.h ,
  https://raw.githubusercontent.com/asivery/epfb-re/master/test.cpp
- On rMPP, the *fastest* mode can be a **silent no-op**: xugro recorded `sendUpdate(…, 0, 3, 1) // this flashes
  screen` vs `sendUpdate(…, 0, 0, 0) // this does nothing (no error too)`. [VS]
  https://github.com/xugro/rmpp-framebuffer
- Generic e-ink waveform semantics: **DU** renders "any graytone to black or white **only**" (no intermediate
  grays); **A2** is fast B&W only; **GC16** is the 16-gray, slow, full-quality mode. [VS]
  https://rmkit.dev/eink-dev-notes/
- reMarkable's own UX confirms fast strokes show grayscale/approximate first and the true content "doesn't get
  filled in until you stop" — i.e. fast waveform ≠ developed content. [VS]
  https://blog.the-ebook-reader.com/2024/09/04/new-remarkable-paper-pro-has-color-e-ink-gallery-3-screen/
- Gallery 3 physically **requires a full flash to change color**: "any change in color requires a full screen
  flash." [VS] https://www.ereadersforum.com/blog/remarkable-paper-pro-move-review-a-focused-device-with-a-very-specific-audience/

So if the auto-path pushes your grayscale-on-white frame with a fast/partial waveform, it legitimately renders
blank/washed-out until a full/GC16-class refresh — and that deferred full refresh is plausibly what you *see*
every ~6 s.

### 1.4 Ranked hypotheses for the ~6 s

| Rank | Hypothesis | Support |
|---|---|---|
| **#1 [INF, strong]** | `libqsgepaper`'s **custom `QAnimationDriver` / deferred-full-refresh** advances present on a coarse cadence (and/or `m_lastUpdateTimer`/`updateCounter` fires a periodic full). | Only place that provably replaces the null driver **and** holds elapsed-time + counter state; only hypothesis explaining *both* fast-scene + periodic-with-no-input. The official guide notes Qt apps redrawing the whole screen get "occasional uncomfortable full-screen refreshes" [VS] https://remarkable.guide/devel/language/c++/qt.html |
| **#2 [VS mechanism, INF for 6 s]** | Panel/EPDC **full-waveform development time** + a count-based ghost-clear, *looking* periodic because the driver pushes at a fixed rate. | GC16/CompleteRefresh on Gallery 3 is intrinsically slow; the standard "full every N updates" idiom is count-based, and the binary has the `updateCounter` primitive. Not 6 s by itself. |
| **#3 [VS it's NOT this]** | A generic **Qt no-vsync fallback** timer. | **Ruled out as the 6 s source:** stock Qt's `requestUpdate` idle default is **5 ms** (`QT_QPA_UPDATE_IDLE_TIME`, fallback `updateInterval=5`) [VS] https://codebrowser.dev/qt6/qtbase/src/gui/kernel/qplatformwindow.cpp.html ; the default animation tick is **16 ms** (`DEFAULT_TIMER_INTERVAL 16`) [VS] https://codebrowser.dev/qt5/qtbase/src/corelib/animation/qabstractanimation.cpp.html . **6 s is no stock Qt default** — a generic fallback spins *fast*, not slow. If 6 s comes from a driver, it's reMarkable's *custom* one → folds into #1. |

**Best-supported synthesis:** the ~6 s is `libqsgepaper`'s **own** behavior — its custom animation driver advancing
present on a coarse cadence and/or an internal timer/counter firing a deferred **full** refresh (the only kind that
makes grayscale-on-white actually appear on Gallery 3). Your 23 ms is sync+render into the off-screen framebuffer;
the panel only visibly resolves on that periodic full/CompleteRefresh pass.

### 1.5 Environment variables

reMarkable / epaper-specific (the only public levers — `libqsgepaper` is **not** env-tunable for refresh):
- `QT_QPA_PLATFORM=epaper` (also accepts `epaper:enable_fonts`). [VS] https://developer.remarkable.com/documentation/qt_epaper
- `QT_QUICK_BACKEND=epaper` — selects the `libqsgepaper` scenegraph backend. [VS] same.
- `QMLSCENE_DEVICE=epaper` — older selector for the QtQuick device backend. [VS] reMarkable env usage.
- **No `QSG_EPAPER_*` / `QT_QPA_EPAPER_*` / `QSGEPAPER_*` env var exists** in any searched source — refresh is
  controlled only by the C++ API. [VS] exhaustive search returned none.

Generic Qt knobs that bear on the symptom (software, non-vsync backend) — worth A/B testing on-device:
- `QSG_RENDER_LOOP=basic|threaded` — force loop type (may be overridden since `libqsgepaper` installs `EPRenderLoop`;
  test whether honored). [VS] https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html
- `QSG_NO_VSYNC=1` — alleviates throttling when vsync is broken/absent. [VS] same.
- `QSG_USE_SIMPLE_ANIMATION_DRIVER=1` (Qt ≥6.5) — switches to an **elapsed-time-based** animation driver that
  "functions correctly even if vsync-based throttling is broken or disabled." **Most promising generic lever**,
  since the prime suspect is the custom animation driver: if this changes the 6 s cadence, hypothesis #1 is
  confirmed. [VS] same.
- `QT_QPA_UPDATE_IDLE_TIME` — overrides the `requestUpdate` idle interval (default 5 ms, *not* seconds). [VS]
  qplatformwindow.cpp above.
- `QSG_RENDER_TIMING=1`, `QSG_INFO=1` (enables `qt.scenegraph.general`) — **diagnostics**: log when sync/render/
  present happen vs. when the panel updates, to confirm empirically whether the gap is between render and present
  (→ animation driver) or after present (→ waveform/full-refresh deferral). [VS] same.

---

## 2. EXACTLY how to force a PROMPT refresh per frame

### 2.1 The API (lowercase-'b' `EPFramebuffer`, exported by `libqsgepaper.so`)

⚠️ **There are two different classes; do not conflate them:**

| | `EPFramebuffer` (lowercase **b**) — **OUR TARGET** | `EPFrameBuffer` (capital **B**) |
|---|---|---|
| Device | **reMarkable Paper Pro (rMPP / ACeP2)** | reMarkable 1 / 2 |
| In | `libqsgepaper.so` (shared) | `libqsgepaper.a` (static, into xochitl) |
| Refresh call | `swapBuffers(QRect, EPContentType, EPScreenMode, QFlags<UpdateFlag>)` | `sendUpdate(QRect, WaveformMode, UpdateMode, bool)` |
| Force-full lever | **`UpdateFlag::CompleteRefresh`** arg (no `setForceFull`) | **`setForceFull(bool)`** + `FullUpdate` |
| RE source | **asivery/epfb-re** | Eeems template-qt-app, canselcik, pl-semiotics |

asivery/epfb-re explicitly states "Right now the rM2 is not supported" — it is the **rMPP** class. [VS]
https://raw.githubusercontent.com/asivery/epfb-re/master/README.TXT . The `setForceFull`/`sendUpdate`/`WaveformMode`
class is rM1/rM2 only. [VS] Eeems header above.

```cpp
// Enums (numeric values reverse-engineered from the rMPP toolchain libqsgepaper.a by asivery). [VS] epfb-re epframebuffer.h
enum EPContentType { Mono = 0, Color = 1 };                                  // mono vs color path (NOT scoped → bare names)
enum EPScreenMode  { QualityFastest = 0, QualityFast = 1, Quality3 = 3,      // waveform quality (NOTE: no value 2)
                     QualityFull = 4, Quality5 = 5 };
class EPFramebuffer {
  enum UpdateFlag { NoRefresh = 0, CompleteRefresh = 1 };                    // CompleteRefresh = forced FULL flash
  unsigned long swapBuffers(QRect changed, EPContentType, EPScreenMode, QFlags<UpdateFlag>);  // present + refresh
  static EPFramebuffer* instance();                                          // the genuine library export
  QImage* getAuxFramebuffer();    // the back buffer you paint INTO
  QImage* getMainFramebuffer();   // the displayed buffer
};
```

### 2.2 From WHICH signal and on WHICH thread

All per-frame `QQuickWindow` signals are documented as **emitted from the scene-graph rendering thread**
(`afterRendering`, `afterFrameEnd`, `frameSwapped`, `before/afterRenderPassRecording`, `beforeRendering`). [VS]
https://doc.qt.io/qt-6/qquickwindow.html . But the docs add "the connected slots are invoked on the scene graph's
dedicated render thread, **if there is one**." [VS] https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html

- The **"basic" (non-threaded) render loop runs the scene graph on the GUI/main thread** — and the software backend
  typically uses basic. So on `basic`, these signals fire on the **GUI thread**. [VS] same (it does not spell out
  the basic case per-signal, but it follows from "if there is one").
- Connect with **`Qt::DirectConnection`** so the slot runs synchronously inside the frame. [VS] qquickwindow.html.

**Recommendation:**
1. Use **`afterRendering`** (Direct) as the per-frame "pixels are ready" hook to call `swapBuffers` after your
   `QQuickPaintedItem` (the WPE BGRA frame) is rasterized into the scene. If you instead need "the previous frame
   is now actually on the panel," use **`frameSwapped`**.
2. Run **`QSG_RENDER_LOOP=basic`** so `afterRendering`/`frameSwapped` fire on the **GUI thread** — the same thread
   that owns your `QCoreApplication`/Qt object graph, and the context `EPFramebuffer` expects ("EPFramebuffer wants
   to run in QT context"; epfb-re drives it via `QTimer::singleShot` for exactly this reason [VS] epfb-re test.cpp).
   This sidesteps render-thread-vs-GUI-thread hazards entirely.
3. If you ever land on the **threaded** loop, your `afterRendering` slot runs on the **render thread** — do not
   touch GUI-thread-affined objects there, and verify `swapBuffers` is happy off the GUI thread. Prefer basic.

### 2.3 Is there a `setForceFull`-equivalent for the lowercase-'b' class? — **No.**

`setForceFull(bool)` belongs to the *other* (capital-'B') class only (`_ZN12EPFrameBuffer12setForceFullEb`). [VS]
Eeems header. The rMPP `EPFramebuffer` header from epfb-re contains **no `setForceFull`, no `m_forceFull`, no
`WaveformMode`, no `sendUpdate`** — its entire surface is `swapBuffers` / `instance` / `getAuxFramebuffer` /
`getMainFramebuffer`. [VS] epfb-re epframebuffer.h. **On rMPP, "force full" = pass `EPScreenMode::QualityFull` +
`UpdateFlag::CompleteRefresh` to `swapBuffers`** — that is the lowercase-'b' equivalent of `setForceFull(true)`.

### 2.4 Does self-driving `swapBuffers` CONFLICT with the scenegraph's auto-present? — **YES, if they overlap.**

This is the single most important safety finding. Community-documented behavior:
> "The Remarkable … has an **undocumented API for partial refreshes** … which is what's behind its magic **that
> disappears when custom Qt applications are used to draw on the screen, even using the toolchain provided by
> Remarkable**." [VS] https://github.com/canselcik/libremarkable/blob/master/reference-material/libqsgepaper.md

The conflict model [INF, well-grounded]: the `epaper` QtQuick scenegraph **already owns the framebuffer and already
presents** (its `EPRenderLoop` auto-drives updates — that's why your Phase-1/3 worked without ever calling
`swapBuffers`). If you *also* grab `instance()` and call `swapBuffers` **on the same pixels while the scenegraph is
running**, you have **two owners of one singleton + one framebuffer** → redundant/competing updates (double-flash,
tearing), waveform fighting (one path partial while the other forces full), and at worst `abort()` — epfb-re's own
`extractPointers()` calls `abort()` if it sees more than one instance of the same framebuffer type. [VS]
https://raw.githubusercontent.com/asivery/epfb-re/master/epfb.cpp . Ownership is also arbitrated by a single
**`QLockFile("/tmp/epframebuffer.lock")`** (`EPFramebuffer::checkLockFile()`). [VS]
https://github.com/Eeems-Org/oxide/blob/master/shared/epaper/epframebuffer.h

**Rule: exactly one presentation owner per pixel region.** No public source documents a *successful* hybrid where a
custom app lets the scenegraph present *and* also calls `swapBuffers` for the same area — treat that as unsupported
until verified on-device. See §5 for the two safe architectures.

---

## 3. The right `(EPContentType, EPScreenMode, UpdateFlag)` triples + a concrete policy

### 3.1 The authoritative rMPP waveform matrix (live re-host of the deleted rmBifrost)

The original `shg8/rmBifrost` is 404 (no Wayback snapshot), but a live re-host exists at `TiagoJMartins/rmBifrost`,
whose `compositor.cpp` gives the **authoritative rMPP waveform-argument matrix** (the three trailing ints map 1:1
onto `swapBuffers(rect, EPContentType, EPScreenMode, UpdateFlag)`). [VS]
https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/main/src/compositor/compositor.cpp (L168–187):

| rmBifrost mode | `(EPContentType, EPScreenMode, UpdateFlag)` | use |
|---|---|---|
| `MONOCHROME`      | `(Mono=0,  0, NoRefresh=0)` | mono partial |
| `COLOR_ANIMATION` | `(Color=1, 0, NoRefresh=0)` | color, animation-fast |
| `COLOR_FAST`      | `(Color=1, 1, NoRefresh=0)` | color, fast |
| `COLOR_1`         | `(Color=1, 3, NoRefresh=0)` | color, mid quality |
| `COLOR_CONTENT`   | `(Color=1, 4, NoRefresh=0)` | color content, no flash |
| `COLOR_2`         | `(Color=1, 5, NoRefresh=0)` | color, highest variant |
| `FULL`            | `(Color=1, 4, CompleteRefresh=1)` | **full anti-ghost / develop flash** |

(The rmBifrost `enum` ordinal `MONOCHROME=0, COLOR_ANIMATION=1, …` is just a local switch label — the **wire values
are the triples above**.) Caveat [INF]: rmBifrost called these via a hard-coded address into xochitl on fw
3.14/3.15; the 3-int **signature/semantics** match epfb-re's `swapBuffers` exactly, so the triples are reusable,
but the exact panel result per `(content, variant, full)` still must be **timed on-device** with epfb-re's harness.

### 3.2 (a) Fast page-turn of mostly-grayscale text — must be fast BUT fully developed

**Recommendation: `swapBuffers(rect, Mono=0, QualityFast=1, NoRefresh=0)`** for the fast partial, paired with the
periodic full flash (§3.4). For the **first** paint of a page (and every Nth), use the full color flash
`(Color=1, QualityFull=4, CompleteRefresh=1)`.

- **Grayscale text does NOT need the Color path** — `Mono` is the correct content type; the panel has a real mono
  path (`MONOCHROME=(0,0,0)`). [VS] rmBifrost compositor.cpp L168–169.
- KOReader's color-e-ink (Kaleido — the closest analog to a Gallery-3 CFA panel) logic confirms the principle:
  text/UI stay on normal **mono/REAGL/GC16** waveforms; it only swaps to a **color waveform when content is flagged
  `dither` AND color is enabled** ("We assume the dither flag is only set on image content"). [VS]
  https://raw.githubusercontent.com/koreader/koreader-base/master/ffi/framebuffer_mxcfb.lua (L362–386). So on a CFA
  color panel, grayscale/text → grayscale waveform; color waveform reserved for real color/image regions. [INF,
  strong]
- **But avoid `QualityFastest=0` for text that must be readable** — fastest can under-develop or be a silent no-op
  (xugro: `(…,0,0,0)` "does nothing"). [VS] https://github.com/xugro/rmpp-framebuffer . Use `QualityFast=1` (or
  `Quality3=3` if text comes up faint), reserving `QualityFastest=0` for transient scroll where slight
  under-development is acceptable.
- **Honest tension to settle on-device:** epfb-re's `test.cpp` displays a static image with `(Color=1,
  QualityFastest=0, CompleteRefresh=1)` — pairing fastest with the *full* flag to guarantee appearance. [VS]
  https://raw.githubusercontent.com/asivery/epfb-re/master/test.cpp . That is consistent with "fastest alone
  under-develops; add the full flag or bump the variant." For a fast turn *without* a flash, `(0,1,0)` is the right
  starting bet.

### 3.3 (b) Occasional full anti-ghost flash — **confirmed `(Color=1, QualityFull=4, CompleteRefresh=1)`**

- rmBifrost `FULL → (1,4,1)`. [VS] compositor.cpp L186–187.
- Gallery 3 physically requires a full flash to change color. [VS] ereadersforum review above. reMarkable documents
  ghosting as normal e-ink behavior cleared by a full refresh. [VS] https://support.remarkable.com/s/article/Ghosting
- **Bonus anti-ghost lever (color-panel only):** rMPP `EPFramebuffer` exposes `ghostControl(GhostControlMode)`,
  reversed by Eeems as `GhostControlMode{ BlinkNow, BlinkLater, BleachNow, FactoryReset }` (`BleachNow`/
  `FactoryReset` ACeP2-only). [VS] https://github.com/Eeems-Org/oxide/blob/master/shared/epaper/epframebuffer.h .
  Not in epfb-re yet → reverse on-device if you want bleach.

### 3.4 Concrete policy (KOReader-style: fast partial per turn + full every N)

**KOReader's verified default `N = 6`:** `DEFAULT_FULL_REFRESH_COUNT = 6`; promotes a partial to full when the
counter reaches `N-1` (`refresh_count = (refresh_count+1) % FULL_REFRESH_COUNT`), and the UI default is "Every 6
pages." [VS] https://raw.githubusercontent.com/koreader/koreader/master/frontend/ui/uimanager.lua (L17, L22–23,
L1163–1174) , https://raw.githubusercontent.com/koreader/koreader/master/frontend/ui/elements/refresh_menu_table.lua
(L68–70).

| Path | Trigger | `swapBuffers` args | = triple |
|---|---|---|---|
| **Fast page-turn (grayscale)** | each turn, counter < N | `(Mono, QualityFast, NoRefresh)` | `(0,1,0)` |
| **Scroll / pan tick** | during motion | `(Mono, QualityFastest, NoRefresh)` | `(0,0,0)` |
| **Small mono UI update** (caret, toolbar, link highlight) | bounded region | `(Mono, QualityFast, NoRefresh)` | `(0,1,0)` |
| **Color region settled** | image/color visible, motion stopped | `(Color, Quality3 / QualityFull, NoRefresh)` | `(1,3,0)` / `(1,4,0)` |
| **Full anti-ghost flash** | every Nth turn **AND** page-load / navigation / color-change | `(Color, QualityFull, CompleteRefresh)` | `(1,4,1)` |

**Starting `N = 6`** (KOReader's default; *lower* than `research-reuse.md`'s earlier 8–12 guess) — Gallery 3 ghosts
more visibly than mono panels, and color page-loads already force a full (resetting the counter), so partial-only
runs are short anyway. **Reset the counter on every full.** **Force a full on every navigation regardless of
counter** (color must flash to develop) — stricter than KOReader, because of the color panel.

**Debounce + waits (steal netsurf + KOReader):**
1. **Coalesce** dirty rects into one bounding box; run an async **debounced redraw on a ~5 Hz / ~200 ms timer** so a
   burst of WPE `buffer-rendered` frames = **one** `swapBuffers` per tick (netsurf-reMarkable design).
2. **8-px align** the changed rect before `swapBuffers` (round x/y down, w/h up to ×8) — E-Ink controller alignment
   (KOReader).
3. **Block animation churn:** inject `* { animation:none!important; transition:none!important;
   scroll-behavior:auto!important }` + emulate `prefers-reduced-motion: reduce` in WPE so CSS doesn't trigger
   endless partials.
4. **Throttle waits:** `swapBuffers` returns a marker (`unsigned long`); only block on completion before the *next
   full*, not on every partial, to keep scrolling fluid (KOReader marker-wait pattern).

---

## 4. Minimal, correct C++ integration snippet

`libqsgepaper` is already loaded by the QPA, but **Qt loads plugins `RTLD_LOCAL`**, so `RTLD_DEFAULT` does **not**
see its symbols — you must `dlopen` the `.so` explicitly to get a handle whose `dlsym` resolves them. [VS]
research-reuse.md §2a (verified on device).

### 4.1 The exact mangled symbols (compiler-verified, not hand-derived)

```
EPFramebuffer::swapBuffers(QRect, EPContentType, EPScreenMode, QFlags<EPFramebuffer::UpdateFlag>)
  →  _ZN13EPFramebuffer11swapBuffersE5QRect13EPContentType12EPScreenMode6QFlagsINS_10UpdateFlagEE

EPFramebuffer::instance()
  →  _ZN13EPFramebuffer8instanceEv
```

Verified by compiling the exact epfb-re declaration with `g++ -std=c++17` (Itanium C++ ABI — the same ABI the
aarch64 reMarkable toolchain uses) and reading the symbol back with `nm` + `c++filt` round-trip. [VS]
local g++/nm/c++filt; input declaration from
https://raw.githubusercontent.com/asivery/epfb-re/master/epframebuffer.h

Component breakdown of the `swapBuffers` name:
| Token | Meaning |
|---|---|
| `_ZN` | nested name (`class::member`) |
| `13EPFramebuffer` | class, length-prefixed (13 chars) |
| `11swapBuffers` | method (11 chars) |
| `E` | end of nested-name qualifier |
| `5QRect` | param 1 `QRect` (by value, 5 chars) |
| `13EPContentType` | param 2 (top-level enum, 13 chars) |
| `12EPScreenMode` | param 3 (top-level enum, 12 chars) |
| `6QFlags I … E` | param 4 `QFlags<…>` template |
| `NS_10UpdateFlagE` | template arg: `S_` = substitution back-ref to `EPFramebuffer`; `10UpdateFlag` (10 chars) ⇒ `QFlags<EPFramebuffer::UpdateFlag>` |

The `S_` back-reference (the nested enum reuses the already-seen `EPFramebuffer` token) is the subtle part — and
exactly why hand-mangling is error-prone. **Verify on the real device** (the export could theoretically live on a
subclass `EPFramebufferSwtcon`):
```sh
# on device (BusyBox): pipe through your toolchain's c++filt
nm -D --defined-only /usr/lib/plugins/scenegraph/libqsgepaper.so | grep -i swapBuffers
```

### 4.2 Constructing the `QFlags<UpdateFlag>` argument

`QFlags<UpdateFlag>` has an implicit constructor from a single flag, so **pass the bare enumerator** — no explicit
`QFlags<…>(…)` wrapper is needed. epfb-re does exactly this: `…, EPFramebuffer::UpdateFlag::CompleteRefresh)`. [VS]
test.cpp. (`QFlags` is a thin wrapper over an `int`; on the ABI the argument is passed as an int-sized value.)

### 4.3 The snippet

```cpp
// epaper_refresh.h  — drive rMPP EPFramebuffer::swapBuffers via dlsym from the already-loaded libqsgepaper.so.
// Build with QT_NO_KEYWORDS (WPE pulls in glib gio whose structs use `signals`/`slots`). Qt6: QRect/QFlags available.
#include <QRect>
#include <QFlags>
#include <dlfcn.h>
#include <cstdio>

// Mirror the reversed enums (numeric values from asivery/epfb-re). Keep these in ONE header.
enum EPContentType { EPC_Mono = 0, EPC_Color = 1 };
enum EPScreenMode  { EPS_QualityFastest = 0, EPS_QualityFast = 1, EPS_Quality3 = 3,
                     EPS_QualityFull = 4, EPS_Quality5 = 5 };
enum EPUpdateFlag  { EP_NoRefresh = 0, EP_CompleteRefresh = 1 };   // QFlags underlying values

class EpaperRefresh {
public:
    bool init() {
        // Qt loaded the plugin RTLD_LOCAL → RTLD_DEFAULT can't see it. dlopen the .so explicitly (RTLD_NOLOAD
        // would also work since it's already mapped; RTLD_NOW|RTLD_GLOBAL is safe and idempotent).
        void* h = dlopen("/usr/lib/plugins/scenegraph/libqsgepaper.so", RTLD_NOW | RTLD_GLOBAL);
        if (!h) { std::fprintf(stderr, "dlopen libqsgepaper: %s\n", dlerror()); return false; }

        instance_  = reinterpret_cast<InstanceFn>(dlsym(h, "_ZN13EPFramebuffer8instanceEv"));
        // swapBuffers takes QFlags<UpdateFlag> by value; on the ABI that is an int-sized value → pass `int`.
        swap_      = reinterpret_cast<SwapFn>(
            dlsym(h, "_ZN13EPFramebuffer11swapBuffersE5QRect13EPContentType12EPScreenMode6QFlagsINS_10UpdateFlagEE"));
        if (!instance_ || !swap_) {
            std::fprintf(stderr, "dlsym EPFramebuffer: %s\n", dlerror());
            return false;   // → fall back to nm -D on device to confirm the exact symbol / subclass
        }
        fb_ = instance_();
        return fb_ != nullptr;
    }

    // Call from QQuickWindow::afterRendering (DirectConnection, GUI thread under QSG_RENDER_LOOP=basic).
    unsigned long present(const QRect& changed, EPContentType c, EPScreenMode m, EPUpdateFlag f) {
        // QFlags<UpdateFlag> is layout-compatible with its underlying int → pass `int(f)`.
        return swap_(fb_, changed, c, m, static_cast<int>(f));
    }

    // Convenience: full color anti-ghost / develop flash over the whole 1620x2160 panel.
    void fullFlash() { present(QRect(0, 0, 1620, 2160), EPC_Color, EPS_QualityFull, EP_CompleteRefresh); }
    // Convenience: fast grayscale page-turn partial.
    void fastMono(const QRect& r) { present(r, EPC_Mono, EPS_QualityFast, EP_NoRefresh); }

private:
    using InstanceFn = void* (*)();
    // First arg = the EPFramebuffer* `this`; QFlags passed as int (see note above).
    using SwapFn     = unsigned long (*)(void* self, QRect, EPContentType, EPScreenMode, int);
    InstanceFn instance_ = nullptr;
    SwapFn     swap_     = nullptr;
    void*      fb_       = nullptr;
};
```

Wiring it to the window (architecture A — add an explicit flash; see §5):
```cpp
EpaperRefresh epaper;                 // member; epaper.init() once after the QML scene is created.
QObject::connect(quickWindow, &QQuickWindow::afterRendering, quickWindow, [&]{
    // Runs on the GUI thread when QSG_RENDER_LOOP=basic. Decide waveform per update class here.
    if (navigationJustHappened) { epaper.fullFlash(); navigationJustHappened = false; refreshCount = 0; }
    else if (++refreshCount >= 6) { epaper.fullFlash(); refreshCount = 0; }    // N = 6 (KOReader default)
    else { epaper.fastMono(alignTo8(lastDirtyRect)); }
}, Qt::DirectConnection);
```
> Notes: (1) passing `QFlags<UpdateFlag>` as `int` relies on `QFlags` being a single-int wrapper (true on every Qt
> ABI) — if you prefer, declare the real `QFlags<UpdateFlag>` type and pass it by value; both ABI-match. (2) If you
> adopt architecture **(B)** (take over the page area), paint the WPE BGRA frame into `fb_`'s `getAuxFramebuffer()`
> (dlsym `_ZN13EPFramebuffer16getAuxFramebufferEv`) instead of feeding the `QQuickPaintedItem`, and keep QML chrome
> in a non-overlapping rect so the scenegraph never presents the page pixels (avoids double-present).

---

## 5. Has anyone driven the rMPP panel from a custom Qt app at speed? + gotchas + (A) vs (B)

### 5.1 Feasibility / ownership

- **Essentially no public precedent for a standalone (non-xochitl) rMPP app at speed** — the working ecosystem
  **injects into xochitl** (XOVI / rm-appload / qtfb) rather than running standalone. Your stop-xochitl +
  `epaper`-QPA path is the **officially documented** one ("Stop the remarkable main application, launch your
  application … then start it again," run `-platform epaper") [VS]
  https://developer.remarkable.com/documentation/qt_epaper — *but that doc covers only rM1/rM2; zero mention of
  Paper Pro, color, or `swapBuffers`*. **Your Phase-1/3 on-device success is itself the standalone proof.**
- **`libqsgepaper` talks to `/dev/dri/card0` directly** (DRM **dumb buffers + atomic KMS**); there is **no
  `/dev/fb0`** on rMPP. [VS] https://github.com/owulveryck/goMarkableStream/issues/117 ,
  https://github.com/Eeems-Org/remarkable.guide/issues/74 (driver `imx-drm`, `DUMB_BUFFER=1`, atomic + universal
  planes, model "reMarkable Ferrari"). xochitl holds 5× `rw-s …/dev/dri/card0` mappings (its dumb-buffer pool).
- **No DRM master/lease machinery** — ownership is implicit single-master, arbitrated by a **lockfile**
  `EPFramebuffer::checkLockFile()` → `QLockFile("/tmp/epframebuffer.lock")`. [VS]
  https://github.com/Eeems-Org/oxide/blob/master/shared/epaper/epframebuffer.h . So a standalone app **can** own
  the panel after `systemctl stop xochitl`, but you must **never run both at once** (lock / DRM-master contention).
  No explicit "drmSetMaster EBUSY" report on rMPP was found — the ecosystem sidesteps it by injecting. [INF]

### 5.2 Buffer ownership / handles

- **You never pass a device handle.** `EPFramebuffer` allocates and owns the buffers itself; the API
  (`instance`, `getAuxFramebuffer()→QImage*`, `getMainFramebuffer()→QImage*`, `swapBuffers`) has **no path/fd
  argument**. [VS] epfb-re README.TXT + epframebuffer.h.
- On rMPP it manages a **triple of QImages** (not a classic 2-buffer pair) backed by card0 dumb buffers; entry
  `setBuffers(std::tuple<QImage,QImage>, QImage* frameBuffer)` "called once during initialization" with offsets
  `frameBuffer@+0xa8 / renderTarget@+0xc0 / previousBuffer@+0xc8`. [VS] oxide epframebuffer.h. rmBifrost
  independently grabs xochitl's fb `QImage*` at instance `+0xc0`, corroborating the offset. [VS]
  https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/main/src/bifrost_impl.cpp (L35).
- **Precondition for `swapBuffers`:** the singleton must be initialized = card0 opened + **waveforms loaded + PMIC
  rails set** — which the `epaper` QPA does at plugin load. xugro captured rMPP bring-up: "Loading waveforms …
  GAL3_…_TC.eink … pmic: setting rails to 6.0,12.0,24.0,-6.0,-12.0,-24.0 … Got instance" then a working flash. [VS]
  https://github.com/Eeems-Org/remarkable.guide/issues/74 + https://github.com/xugro/rmpp-framebuffer . **Gap [INF]:**
  the only public end-to-end bring-up from a non-xochitl process is the `epaper` QPA plugin itself (xugro's flash
  ran via `LD_PRELOAD …/usr/bin/xochitl`, i.e. xochitl had already brought the panel up). Since you load the QPA
  with xochitl stopped, the QPA performs the bring-up — your standalone success confirms this works.
- epfb-re's `createControlledInstance()` is an **`LD_PRELOAD`/`RTLD_NEXT` interposer** that hooks the `QImage` ctor
  to *discover* xochitl's buffers — it is **not** a card0 opener, and it's meant for the **inject** case. [VS]
  epfb.cpp. **For a standalone app, use plain `instance()` + `getAuxFramebuffer()`** — verify it resolves on your
  fw (it should, since the QPA already initialized the singleton).

### 5.3 Known transient failure modes (none permanently brick the panel — all ssh-recoverable)

- **Wrong waveform args → silent no-op / color "doesn't develop":** xugro `(…,0,0,0)` "does nothing (no error
  too)." [VS] xugro/rmpp-framebuffer. → use `QualityFast`/`Quality3`, not `QualityFastest`, for must-see content.
- **Raw `/dev/dri/card0` bypassing `libqsgepaper` → heavy ghosting / stuck-white:** Jayy001 kmscube — "ghosting will
  occur a lot. The white sphere will eventually go away with enough refreshes (can force this by sleeping the
  device)." [VS] https://github.com/Jayy001/rmpp-kms-cube/blob/main/README.md . **Strong reason NOT to hand-roll
  DRM — use `libqsgepaper`'s waveform engine.** (rMPP uses a software TCON "Swtcon", *not* an FPGA bridge:
  `EPFramebuffer::Backend{Tcon, Swtcon, Desktop}`, rMPP=Swtcon. [VS]
  https://github.com/Eeems-Org/oxide/blob/master/shared/libblight_client/qt.h — so no FPGA reload to worry about.)
- **Injection-path (qtfb / rm-appload) bugs** (argues *for* the standalone approach): white screen (#13), full
  refresh impossible via qtfb (#26), yellow tint / uncontrollable ghost-bust (#19), dash/ghosting while writing
  (#33), "Failed to open the framebuffer" (#32), xochitl SIGSEGV on exit (#10), **firmware-bump boot-loop** on
  3.27.1.0 (#57 — the brittle-hook failure). [VS] https://github.com/asivery/rm-appload/issues/{13,19,26,32,33,57,10}
- **xochitl vetoes an unsupported color mode (SIGABRT) while it still owns the panel:** "No such mode supported:
  N_RGB565 … exit code 6." [VS] https://github.com/koreader/koreader/issues/14528 — mostly N/A once xochitl is
  stopped, but a reminder to send only supported `(content, variant)` combos.
- **Always restore xochitl on exit** so a bad/half-developed frame can't persist.

### 5.4 (A) Scenegraph presents + add a flash  vs  (B) take over the page area

The scenegraph **does** auto-drive updates (`EPRenderLoop`: `maybeUpdate`/`handleUpdateRequest`,
`EPRectangleNode::updateIsGrayscale`) [VS]
https://github.com/canselcik/libremarkable/blob/master/reference-material/libqsgepaper.md — so plain mono refreshes
without an explicit call. **But for color it picks a generic/partial mode → color under-develops / ghosts** (why
you already need a forced full). [INF, strong]

**(A) Let the scenegraph present; only ADD a periodic/explicit full flash via `swapBuffers`.**
- **Pros:** minimal change to working Phase-3b code; QtQuick keeps doing layout/compositing/mono partials; chrome
  refreshes normally; lowest risk of a fully white screen.
- **Cons:** **no per-update waveform control for the page area** — you only get the scenegraph's generic mode + your
  coarse full flashes; color regions still ghost between flashes. **Double-present risk** if both the scenegraph and
  your code hit overlapping regions → mitigate by flashing **full-screen only after motion settles** and letting the
  scenegraph own partials.

**(B) Take over the page area: paint WPE BGRA into `getAuxFramebuffer()` and call `swapBuffers` yourself.**
- **Pros:** **full per-update-class waveform control** — exactly the §3.4 policy. This is the proven recipe
  (epfb-re `test.cpp`: `QPainter(framebuffer->getAuxFramebuffer()); drawImage(...); swapBuffers(rect, Color, …,
  CompleteRefresh)` [VS] test.cpp; rmBifrost is a full standalone compositor over the same call [VS] compositor.cpp).
  WPE already hands you a full-screen **BGRA == `QImage::Format_ARGB32`** buffer (no conversion), so writing it into
  the aux QImage is cheap.
- **Cons:** you must **bypass the QtQuick scenegraph for the page region** or it will *also* present → double-present.
  Keep chrome either in a small QML layer that never overlaps the page rect, or draw it yourself into the same aux
  buffer. `getAuxFramebuffer`/`instance` must resolve standalone on your fw (epfb-re's `createControlledInstance`
  uses LD_PRELOAD ctor hooks for the *inject* case; plain `instance()` should suffice — **verify on device**).
  `EPFramebuffer` wants a `QCoreApplication`/event-loop context (epfb-re calls via `QTimer::singleShot`). [VS]
  test.cpp.

**Recommendation:** ship **(A)** first (you already have it — just add the `(1,4,1)` flash on navigation + every
Nth partial), then move the page area to **(B)** for real waveform control once `instance()`/`getAuxFramebuffer()`
are confirmed to resolve standalone on-device. **Either way: exactly one owner per pixel region.**

---

## 6. Recommended on-device experiment (disambiguate the cause + validate the fix)

1. Run with `QSG_RENDER_TIMING=1 QSG_INFO=1` and watch whether render/present log every ~23 ms while the panel
   lags. Gap *before* present → animation driver / present coalescing; gap *after* present → waveform / full-refresh
   deferral.
2. Toggle one at a time: `QSG_USE_SIMPLE_ANIMATION_DRIVER=1`, then `QSG_NO_VSYNC=1`, then `QSG_RENDER_LOOP=basic`.
   **If `QSG_USE_SIMPLE_ANIMATION_DRIVER=1` changes the ~6 s cadence → the custom animation driver (hypothesis #1)
   is confirmed as the throttle.**
3. Independently, after `update()`, call `swapBuffers(fullRect, Color, QualityFull, CompleteRefresh)` yourself (per
   §4). **If frames now appear immediately → the deferred-full-refresh-of-grayscale-on-white (§1.3) is confirmed as
   the visibility half**, and the explicit-refresh recipe is the fix.
4. Confirm the mangled symbol on the real lib: `nm -D --defined-only /usr/lib/plugins/scenegraph/libqsgepaper.so |
   grep -i swapBuffers` (pipe through the toolchain `c++filt`).

---

## 7. Deltas to fold back into `docs/research-reuse.md` §2/§2a

1. **The throttle is identified [new]:** `libqsgepaper` ships a custom `EPRenderLoop` + custom `QAnimationDriver`
   (exports `EPContext::createAnimationDriver`, `EPRenderLoop::animationDriver`) — that, on a no-vsync software
   backend, is the prime cause of the ~6 s panel cadence (not any stock Qt default; Qt's idle is 5 ms, anim tick
   16 ms). Generic knobs to test: `QSG_USE_SIMPLE_ANIMATION_DRIVER=1`, `QSG_NO_VSYNC=1`, `QSG_RENDER_LOOP=basic`,
   `QSG_RENDER_TIMING=1`.
2. **Full rmBifrost matrix is LIVE (not gone)** at `TiagoJMartins/rmBifrost`, and richer than the doc's copy — add
   `COLOR_ANIMATION=(1,0,0)`, `COLOR_1=(1,3,0)`, `COLOR_2=(1,5,0)` alongside the known `MONOCHROME/COLOR_FAST/
   COLOR_CONTENT/FULL`.
3. **`FULL_REFRESH_COUNT` default `N = 6`** (KOReader, verified) — lower the doc's "8–12" guess to **start at 6**.
4. **Grayscale text → the `Mono` path**, not Color (KOReader keeps text on grayscale waveforms; the color waveform
   is reserved for `dither`+color/image regions). Avoid `QualityFastest` for must-see text (silent-no-op risk) —
   use `QualityFast`/`Quality3`.
5. **Self-driving `swapBuffers` is only safe with exactly ONE owner per pixel region** — the "magic disappears with
   custom Qt apps" effect is double-present / waveform-fighting; epfb-re even `abort()`s on duplicate instances.
   Ownership is arbitrated by `QLockFile("/tmp/epframebuffer.lock")`; **never run alongside xochitl**.
6. **rMPP = `/dev/dri/card0` DRM dumb buffers + atomic KMS, no `/dev/fb0`, software TCON "Swtcon" (no FPGA bridge),
   implicit single DRM-master (no leases).** `EPFramebuffer` owns its own buffers (no fd argument); the `epaper` QPA
   does the waveform/PMIC bring-up at plugin load (works with xochitl stopped — your standalone proof).
7. **Raw-DRM bypass ghosts badly** (Jayy001 kmscube) → stay on `libqsgepaper`'s waveform engine, do **not**
   hand-roll `/dev/dri/card0`.
8. **`createControlledInstance` is an LD_PRELOAD interposer for the INJECT case** — for standalone use plain
   `instance()` + `getAuxFramebuffer()` and verify resolution on-device.
9. **Compiler-verified mangled symbol** for the present call:
   `_ZN13EPFramebuffer11swapBuffersE5QRect13EPContentType12EPScreenMode6QFlagsINS_10UpdateFlagEE` (and
   `_ZN13EPFramebuffer8instanceEv`). Pass `QFlags<UpdateFlag>` as the bare enumerator (implicit ctor) / int.
10. **Call from `QQuickWindow::afterRendering` (Direct), GUI thread under `QSG_RENDER_LOOP=basic`.** There is **no
    `setForceFull` on the lowercase-'b' class** — "force full" = `QualityFull` + `CompleteRefresh`.

---

## 8. Source index (all primary, raw where possible)

- **rMPP refresh API (reversed):** asivery/epfb-re — `epframebuffer.h`, `test.cpp`, `epfb.cpp`, `README.TXT`,
  `OLD/modetest.cpp` · https://github.com/asivery/epfb-re
- **rMPP waveform matrix + fb offset:** TiagoJMartins/rmBifrost (re-host of deleted shg8/rmBifrost) —
  `src/compositor/compositor.cpp` (L168–187), `src/bifrost_impl.cpp` (L35)
- **`libqsgepaper` symbol dump / EPRenderLoop / EPRectangleNode:** canselcik/libremarkable
  `reference-material/libqsgepaper.md`
- **rM1/rM2 `EPFrameBuffer` (timer/counter/setForceFull — the *other* class):**
  Eeems-Org/remarkable-template-qt-app `src/vendor/epaper/epframebuffer.h`
- **DRM card0 / no fb0 / lockfile / Swtcon / GhostControlMode / buffer offsets:** Eeems-Org/oxide
  `shared/epaper/epframebuffer.h`, `shared/libblight_client/{drm.cpp,qt.h}` · owulveryck/goMarkableStream #117 ·
  Eeems-Org/remarkable.guide #74
- **rMPP bring-up / "does nothing" no-op:** xugro/rmpp-framebuffer
- **Raw-DRM ghosting:** Jayy001/rmpp-kms-cube
- **QPA `flush()` no-ops (red herring):** reMarkable/qt5-qpa-epaper `qminimalbackingstore.cpp` ·
  reMarkable/epaper-qpa `epaperbackingstore.cpp`
- **Upstream Qt basic loop (mirror; `animationDriver()==nullptr`):** code.qt.io qtdeclarative
  `qsgrenderloop.cpp` · `requestUpdate`=5 ms codebrowser qtbase `qplatformwindow.cpp` · anim tick 16 ms
  codebrowser qtbase `qabstractanimation.cpp`
- **Qt6 signals/threads + scenegraph env vars:** doc.qt.io `qquickwindow.html`,
  `qtquick-visualcanvas-scenegraph.html`
- **KOReader policy:** koreader/koreader `frontend/ui/uimanager.lua` (`DEFAULT_FULL_REFRESH_COUNT=6`),
  `frontend/ui/elements/refresh_menu_table.lua` (Every-6-pages default) · koreader/koreader-base
  `ffi/framebuffer_mxcfb.lua` (Kaleido color/grayscale waveform selection)
- **Injection-path bugs (argue for standalone):** asivery/rm-appload issues #10/#13/#19/#26/#32/#33/#57
- **Gallery 3 needs a flash for color:** ereadersforum.com rMPP Move review · blog.the-ebook-reader.com rMPP
  Gallery-3 · support.remarkable.com Ghosting
- **e-ink waveform semantics (DU/A2/GC16):** rmkit.dev/eink-dev-notes
- **Official stop-xochitl recipe (rM1/2 only):** developer.remarkable.com/documentation/qt_epaper
- **rMPP color palette/dither/ICC (no refresh-timing data):** thregr.org/wavexx/rnd/20260201-remarkable_pro_colors
