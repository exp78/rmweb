# WPE WebKit 2.48 — headless frame cadence: why frames come on a ~6 s grid, and how to pull them on demand

**Scope.** We embed a high‑level `WebKitWebView` on a **headless** `WPEDisplay`
(`wpe_display_headless_new()` → `WPEViewHeadless`), render **entirely on CPU** (Skia software;
`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`, `WEBKIT_SKIA_CPU_PAINTING_THREADS=0`,
`WEBKIT_DISABLE_ASYNC_SCROLLING=1`, `JSC_useJIT=0`), and consume frames from the WPEView
`"buffer-rendered"` signal (`wpe_buffer_shm_get_data`). The symptom: `"buffer-rendered"` fires on a
near‑perfect **~6.0 s cadence even with no input** (measured 513, 6757, 12774, 18768, 24760, 30925 ms),
and a JS‑driven scroll repaint (`window.scrollBy` + a 1‑char DOM mutation) finishes in ~8 ms but the new
buffer is not emitted until the next ~6 s grid tick (measured flip‑latency 3553 ms). The render itself is
fast (~8–25 ms). We need frames **on demand**, immediately after a scroll.

**Authority.** Every WPE/WebKit claim is checked against the **actual 2.48.5 source tree** unpacked at
`build/src/wpewebkit-2.48.5 (repo-local, gitignored)` (matches upstream
`wpewebkit-2.48.5`); file:line citations are to that tree. Public‑API signatures are from the installed
dev headers under `build/stage/usr/include/wpe-webkit-2.0/`. Tags: **[VS]** = verified in local source/headers;
**[WEB]** = external doc/commit (URL given); **[INF]** = reasoned inference; **UNVERIFIED** = stated but not
yet proven on this device. Companion docs: `wpe-rendering-protocol.md` (buffer lifecycle / mapping),
`remarkable-eink-refresh.md` (the *separate* panel‑present ~6 s on the epaper/QSG side — **do not conflate**).

---

## 0. TL;DR

1. **There is NO ~6 s timer anywhere in WPE's render pipeline.** [VS] We traced every stage
   (WebProcess `LayerTreeHost` → UIProcess `AcceleratedBackingStoreDMABuf` → `WPEViewHeadless`) and the
   WebProcess display‑refresh side (`DisplayLink` + `DisplayVBlankMonitor`). For a **mapped, active** page,
   frame production is **fully on demand** and paced at up to **60 fps**. The headless view's own frame
   timer is **1/60 s** (`G_USEC_PER_SEC / 60`), not 6 s — `WPEViewHeadless.cpp:122` [VS].
2. **The WebKit render clock here is a 60 fps software timer — confirmed by source, not assumed.** [VS]
   Our app creates exactly **one** `WPEDisplay` (the headless one), and "the first `WPEDisplay` created is
   set as primary" (`WPEDisplay.cpp:88‑90,218`); the headless display has **no `WPEScreen`**, so
   `ScreenManager::primaryDisplayID()` is **0** (`ScreenManagerWPE.cpp:69`), and with `displayID==0`
   `DisplayVBlankMonitor::create()` returns the **`DisplayVBlankMonitorTimer` at 60 fps**
   (`DisplayVBlankMonitor.cpp:47`). So the WebProcess rendering‑update clock runs at **60 fps**, fully
   on‑demand. **Therefore the ~6 s `buffer-rendered` cadence is NOT the WebKit DisplayLink** (that would be
   ≤16 ms). The residual ~6 s is almost certainly the **e‑ink/QSG panel‑present loop** documented in
   `remarkable-eink-refresh.md` (a separate clock), and `WEBKIT_FORCE_VBLANK_TIMER` will likely be a **no‑op**
   here (we are already on the timer). See §1.4 for the full chain and the one residual unknown. [VS][INF]
3. **The page is NOT throttled by activity state.** [VS] A mapped headless view with its `WPEToplevelHeadless`
   (which is constructed **ACTIVE**) yields `IsVisible + IsInWindow + WindowIsActive` →
   `isVisibleAndActive() == true`, so `ThrottlingReason::VisuallyIdle`/`OutsideViewport` are **not** set.
   Earlier "the embedded view is visually idle" theories are **wrong** for this setup (§1.3).
4. **WPE 2.48 has no `webkit_web_view_get_snapshot()` and no `wpe_view_set_target_refresh_rate()`** [VS]
   (those are GTK‑/screen‑only). The available knobs are narrow (§2, §3).
5. **Recommended action (ranked in §5):** first **re‑read the existing `flip-latency` logs in isolation**
   (§6 Probe 2) to classify whether the *`buffer-rendered` signal itself* is on the 6 s grid (**A2**, a real
   WPE gate) or only the **panel** lags (**A1**, the likely case — the fix is the epaper present path, which
   the app already drives via `EpaperRefresh`). Keep the DOM‑mutation repaint trigger (the only public way to
   force a frame on WPE — no `forceRepaint`, no `get_snapshot`). `WEBKIT_FORCE_VBLANK_TIMER=1` is a one‑shot
   **control**, expected to be a no‑op. See §5.

---

## 1. Root cause — the full, source‑level trace

### 1.1 The headless view emits at 60 fps, not 6 s

When a buffer is submitted to the headless view, it is paced by a GLib frame‑timer source at **1/60 s**:

`Source/WebKit/WPEPlatform/wpe/headless/WPEViewHeadless.cpp:115‑130` [VS]:
```cpp
static gboolean wpeViewHeadlessRenderBuffer(WPEView* view, WPEBuffer* buffer, ...) {
    auto* priv = WPE_VIEW_HEADLESS(view)->priv;
    priv->pendingBuffer = buffer;
    auto now = g_get_monotonic_time();
    if (!priv->lastFrameTime) priv->lastFrameTime = now;
    auto next = priv->lastFrameTime + (G_USEC_PER_SEC / 60);   // 16.67 ms target
    priv->lastFrameTime = now;
    if (next <= now) g_source_set_ready_time(priv->frameSource.get(), 0); // fire now
    else             g_source_set_ready_time(priv->frameSource.get(), next);
    return TRUE;
}
```
The timer's dispatch then emits the signal — `WPEViewHeadless.cpp:87‑93`:
`wpe_view_buffer_rendered(view, committedBuffer)`. **Conclusion:** the headless view is NOT the gate. In
fact note the subtlety: after an idle gap, `lastFrameTime` is stale (from the previous frame), so on the
*first* submitted buffer `next <= now` and it fires **immediately** (`ready_time = 0`). So the headless
view adds **≤16 ms**, never seconds. The 6 s gate is strictly **upstream of `render_buffer`** — i.e. the
question is *when does WebKit decide to produce and commit a frame at all*. [VS][INF]

### 1.2 The producer chain is fully on‑demand (no periodic timer)

The buffer that reaches `render_buffer` originates in the WebProcess compositor and is shipped to the
UIProcess, which calls `wpe_view_render_buffer`:

- **UIProcess `AcceleratedBackingStoreDMABuf`** — `Source/WebKit/UIProcess/wpe/AcceleratedBackingStoreDMABuf.cpp`
  [VS]: `frame(bufferID, damage, fence)` (from the WebProcess) → `renderPendingBuffer()` →
  `wpe_view_render_buffer(m_wpeView, ...)` (`:163`) → on `buffer-rendered` → `bufferRendered()` (`:177`) →
  `frameDone()` (`:171`, acks the WebProcess so it may send the next frame). **No timer here** — strictly
  event‑driven, one frame per WebProcess `frame()`.
- **WebProcess `LayerTreeHost`** (the CoordinatedGraphics layer‑tree host that produces frames) —
  `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/LayerTreeHost.cpp` [VS]:
  - `scheduleLayerFlush()` arms `m_layerFlushTimer.startOneShot(0_s)` — i.e. **immediate**, `:150‑167`.
  - `layerFlushTimerFired()` → `flushLayers()` → `page->updateRendering()` (the WebCore rendering update:
    rAF, animations, IntersectionObserver, then paint) → commits a new buffer **only if something changed**
    (`m_compositionRequired || m_pendingResize || m_forceFrameSync || didChangeSceneState`), `:218‑220`.
  - There is **no fixed‑interval timer** in this class. A flush is only scheduled when WebCore requests a
    rendering update (`triggerRenderingUpdate()` → `scheduleLayerFlush()`,
    `DrawingAreaCoordinatedGraphics.cpp:317‑326` [VS]).

So with a **static** page (our test page runs one `<script>` once; no `requestAnimationFrame` loop), nothing
schedules a flush, and **no frames should flow at all**. That the app sees a frame every ~6 s with zero input
means *something outside the WebCore "content changed" path is poking the rendering‑update clock*. [VS][INF]

### 1.3 The page is visible+active — NOT throttled by activity state

A popular theory is "the headless view is `VisuallyIdle`/`OutsideViewport`, so WebKit throttles to a slow
interval." **This is false here.** [VS] The headless display's `create_view` attaches a
`WPEToplevelHeadless`, and that toplevel is constructed **ACTIVE**:

`Source/WebKit/WPEPlatform/wpe/headless/WPEToplevelHeadless.cpp:39‑44` [VS]:
```cpp
static void wpeToplevelHeadlessConstructed(GObject* object) {
    ...
    wpe_toplevel_state_changed(WPE_TOPLEVEL(object), WPE_TOPLEVEL_STATE_ACTIVE);
}
```
`Source/WebKit/WPEPlatform/wpe/headless/WPEDisplayHeadless.cpp:87‑93` [VS]: `create_view` does
`wpe_view_set_toplevel(view, toplevel)` with that ACTIVE toplevel.

The WebKit glue then sets the activity bits from those facts —
`Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp:67‑77` [VS]:
```cpp
if (wpe_view_get_mapped(m_wpeView.get()))         m_viewStateFlags.add(IsVisible);
if (wpe_view_get_has_focus(m_wpeView.get()))      m_viewStateFlags.add(IsFocused);
if (auto* toplevel = wpe_view_get_toplevel(...)) {
    m_viewStateFlags.add(IsInWindow);
    if (wpe_toplevel_get_state(toplevel) & WPE_TOPLEVEL_STATE_ACTIVE)
        m_viewStateFlags.add(WindowIsActive);     // ← set, because the headless toplevel is ACTIVE
}
```
Our app maps the view (`wpe_view_get_mapped()==TRUE`, logged at startup in `main.cpp:342`), so we get
**IsVisible + IsInWindow + WindowIsActive** (focus is the only missing bit, which is irrelevant to the
rendering‑update rate). Therefore `Page::isVisibleAndActive()==true`
(`Source/WebCore/page/Page.cpp:3206` [VS]) and `m_throttlingReasons` does **not** contain `VisuallyIdle`
or `OutsideViewport`. The throttle constants below are therefore **not in play**:

`Source/WebCore/platform/graphics/AnimationFrameRate.h:49‑56` [VS]:
```cpp
constexpr Seconds FullSpeedAnimationInterval            { 15_ms };   // 60 fps
constexpr Seconds HalfSpeedThrottlingAnimationInterval  { 30_ms };   // VisuallyIdle/LowPowerMode → 30 fps
constexpr Seconds AggressiveThrottlingAnimationInterval { 10_s  };   // OutsideViewport
```
(Even if they *were* in play, the worst case is `OutsideViewport` → a **10 s** interval, not 6 s; and
`VisuallyIdle` only halves to 30 fps. None of these equals ~6 s. [VS]) So **activity‑state throttling is
ruled out** as the cause.

### 1.4 What the ~6 s actually is: the rendering‑update clock (DisplayLink/vblank)

For a **mapped, active** page the rate of *rendering updates* (and thus the maximum frame rate, and the
latency before a requested repaint is delivered) is governed by the **DisplayLink**, because this build has
`HAVE(DISPLAY_LINK)` **defined** for WPE — `Source/WTF/wtf/PlatformHave.h:350‑352` [VS]:
```cpp
#if PLATFORM(MAC) || PLATFORM(GTK) || PLATFORM(WPE)
#define HAVE_DISPLAY_LINK 1
#endif
```
Flow [VS]:
1. WebCore schedules an animation/rendering update → `RenderingUpdateScheduler::scheduleAnimation()` →
   `DisplayRefreshMonitorManager::scheduleAnimation()` → `WebDisplayRefreshMonitor::startNotificationMechanism()`
   sends `Messages::WebProcessProxy::StartDisplayLink(observerID, displayID(), maxPreferredFPS or 60)` to the
   UIProcess — `Source/WebKit/WebProcess/WebPage/WebDisplayRefreshMonitor.cpp:78‑101`.
2. UIProcess `DisplayLink` for that `displayID` ticks on a **`DisplayVBlankMonitor`** and calls
   `displayLinkFired()` back into the WebProcess, which drives `updateRendering()` and ultimately a buffer.
   The DisplayLink stops itself ~20 frames after the last observer leaves
   (`maxFireCountWithoutObservers { 20 }`, `Source/WebKit/UIProcess/DisplayLink.cpp:42,219‑226`) — i.e.
   it is *not* a free‑running periodic source.

**Which vblank monitor?** `DisplayVBlankMonitor::create(displayID)` —
`Source/WebKit/UIProcess/glib/DisplayVBlankMonitor.cpp:44‑58` [VS]:
```cpp
static const char* forceTimer = getenv("WEBKIT_FORCE_VBLANK_TIMER");
if (!displayID || (forceTimer && strcmp(forceTimer, "0")))
    return DisplayVBlankMonitorTimer::create();          // ← software 60 fps
#if USE(LIBDRM)
    if (auto monitor = DisplayVBlankMonitorDRM::create(displayID)) return monitor;  // ← real DRM vblank
#endif
return DisplayVBlankMonitorTimer::create();
```
- The **timer** monitor runs at `FullSpeedFramesPerSecond` (60) — `DisplayVBlankMonitorTimer.cpp:40‑49` [VS]
  (`sleep_for(1000/60 ms)`).
- The **DRM** monitor blocks on `drmWaitVBlank`/page‑flip of the CRTC bound to the screen, at the CRTC's mode
  refresh rate — `DisplayVBlankMonitorDRM.cpp` [VS].

**The pivot is `displayID`.** The headless display has **no `WPEScreen`** (`WPEDisplayHeadless` does not
override `get_n_screens`/`get_screens`; there is no `WPEScreenHeadless` in the tree) [VS]. So the WebKit glue
falls back to `ScreenManager::primaryDisplayID()` — `WPEWebViewPlatform.cpp:79‑82` [VS] — and the WPE
`ScreenManager` computes the primary id from the **primary display's screens**:
`Source/WebKit/UIProcess/wpe/ScreenManagerWPE.cpp:63‑70` [VS]:
```cpp
auto screensCount = wpe_display_get_n_screens(display);
auto* screen = screensCount ? wpe_display_get_screen(display, 0) : nullptr;
m_primaryDisplayID = screen ? displayID(screen) : 0;     // ← 0 when there are no screens
```

**Which `displayID` do WE get? — resolved from source: `0`.** [VS] `wpe_display_get_primary()` returns
`s_primaryDisplay`, which is set to **the first `WPEDisplay` ever constructed** in the process —
`WPEDisplay.cpp:87‑90`:
```cpp
static void wpeDisplayConstructed(GObject* object) {
    if (!s_primaryDisplay) s_primaryDisplay.reset(WPE_DISPLAY(object));   // first-created wins
    ...
}
```
and the doc comment confirms "By default, the first #WPEDisplay that is created is set as primary"
(`WPEDisplay.cpp:218`). **Our app creates exactly one `WPEDisplay` — the headless one** (`main.cpp:301`,
`wpe_display_headless_new()`), before any WebKit internals run. (Both the DRM and Wayland platform
implementations register at the *same* GIO extension priority as headless (`-100`/`0`) — `WPEDisplayDRM.cpp:70`,
`WPEDisplayWayland.cpp:97` [VS] — but a platform implementation is only *instantiated* if something asks for a
default display; nothing in our flow does, because we hand WebKit an explicit headless `"display"`.) So
`s_primaryDisplay` **is** our headless display, it has **no screens**, and therefore:

> **`primaryDisplayID() == 0` → `DisplayVBlankMonitorTimer` @ 60 fps.** The WebKit rendering‑update clock is a
> 60 fps software timer, **fully on demand**. This is **world A, and it is confirmed by source — not assumed.**

**Consequence:** the ~6 s `buffer-rendered` cadence is **not** the WebKit DisplayLink (which would deliver a
requested repaint in ≤16 ms). Two possibilities remain for the residual ~6 s, both *outside* the WPE render
clock:

| | What | Evidence | Status |
|---|---|---|---|
| **A1 — epaper panel loop perceived as frames** | The only ~6 s clock proven to exist in this app is the **`libqsgepaper` `EPRenderLoop`/panel‑present** cadence (`remarkable-eink-refresh.md`). If the engine's frame‑count logging is being read off something coupled to that loop, the "~6 s" is the panel loop, not WPE producing buffers. | `remarkable-eink-refresh.md` §1 [VS for the QSG loop existence] | **most likely** [INF] |
| **A2 — an unidentified periodic rendering‑update trigger in the WebProcess** | Something re‑arms `scheduleRenderingUpdate()` ~every 6 s for a *static* page (we ruled out activity‑state throttling, the layer‑flush timer, the DisplayLink free‑run, and the opportunistic GC scheduler as the *source*). No 6 s constant exists in the tree. | exhaustive grep: no `6_s`/`6000`‑class constant in the render path [VS] | **possible, unconfirmed** [INF] |

> **Actionable consequence:** because we are already on the 60 fps timer, `WEBKIT_FORCE_VBLANK_TIMER=1` is
> expected to be a **no‑op** for the cadence — it is still worth running **once** as a definitive control
> (if it changes nothing, world A is nailed and the search moves entirely to the epaper side; the §6 device
> probe settles it in one run). The real fix for "page turns feel slow" is then **not** in WPE at all but in
> the panel‑present path (`remarkable-eink-refresh.md`: drive `EPFramebuffer::swapBuffers` per page‑turn) —
> which the app **already does** via `EpaperRefresh` (`main.cpp:2459‑2526`). If page turns are *still* slow
> after that, the remaining suspect is A2; instrument per §6.

### 1.5 The scroll‑flip latency, reconciled with world A

`window.scrollBy` + the DOM mutation *does* schedule a rendering update (the mutation dirties layout/paint →
`scheduleRenderingUpdate()` → a DisplayLink observer is added → next tick delivers it). In **world A** that
tick is ~16 ms away, so **WPE should emit the new SHM buffer in ≤16 ms.** The measured **3553 ms
flip‑latency** therefore is *not* the time for WPE to produce the buffer — it is best read as the time until
the change is **visible on the e‑ink panel**, which is gated by the panel‑present loop (~6 s grid), i.e. the
buffer is produced quickly but the panel doesn't show it until the next epaper present. [INF, strong]

> **Caveat / residual unknown (A2):** if instrumentation shows the `buffer-rendered` *signal itself* (not the
> panel) firing only every ~6 s after a scroll, then the DisplayLink for `displayID 0` is **not ticking at
> 60 fps** as the source implies, and there is an unidentified gate (A2). The §6 probe (log the actual
> `buffer-rendered` timestamps with and without `WEBKIT_FORCE_VBLANK_TIMER=1`, independent of the panel)
> distinguishes "WPE is slow" (A2) from "panel is slow" (A1). The current engine logging
> (`flip-latency` measured at `onBuffer`, `main.cpp:1548‑1550`) **does** time the signal, so re‑reading those
> logs in isolation answers this directly. [INF]

Note `WEBKIT_DISABLE_ASYNC_SCROLLING` is already set (read at `DrawingAreaCoordinatedGraphics.cpp:218` [VS]);
it keeps scrolling on the main rendering path but does not change pacing.

---

## 2. Every force‑render / refresh‑rate / throttle knob (with ground truth)

| Knob | Type | Exact name / signature | Effect | Source |
|---|---|---|---|---|
| **Force timer vblank** | env | `WEBKIT_FORCE_VBLANK_TIMER` (set to non‑`"0"`) | Forces `DisplayVBlankMonitorTimer` (software **60 fps**) instead of the DRM page‑flip monitor. **Diagnostic control here:** we already resolve to `displayID 0` → the timer monitor (§1.4), so this is expected to be a **no‑op**; it only matters if a DRM display unexpectedly became primary (world B). | `DisplayVBlankMonitor.cpp:46‑48` [VS] |
| **Screen refresh rate** | API | `void wpe_screen_set_refresh_rate(WPEScreen*, int refresh_rate)` / `int wpe_screen_get_refresh_rate(WPEScreen*)` | Sets the per‑screen refresh rate used to derive nominal fps and (in world B) the DRM monitor's rate. **Units are milli‑Hertz** (so 60 Hz = `60000`): documented "The refresh rate of the screen in milli‑Hertz" and the DRM view divides by 1000 to get Hz (`Seconds(1 / (wpe_screen_get_refresh_rate(...) / 1000.))`). **But the headless display has no `WPEScreen`, so there is nothing to call this on** unless you implement a screen. | header `WPEScreen.h:70‑72` [VS]; units `WPEScreen.cpp:252` + `WPEViewDRM.cpp:94` [VS]; usage `DisplayVBlankMonitorDRM.cpp:111` [VS]; `ScreenHeadless` absent [VS] |
| **Disable async scrolling** | env | `WEBKIT_DISABLE_ASYNC_SCROLLING` | Keeps scrolling synchronous on the main path. Already set by us. Does **not** change vblank pacing. **Note the inverted‑logic caveat** [WEB]: the value's meaning differs by DrawingArea variant — the non‑GLib `DrawingAreaCoordinatedGraphics` disables when value `!= "0"` (so `=1` disables), while the GLib variant disables when `== "0"`. We use the non‑GLib path (read at `:218`), so `=1` is correct for us. | `DrawingAreaCoordinatedGraphics.cpp:218` [VS] |
| **Single‑threaded CPU paint** | env | `WEBKIT_SKIA_CPU_PAINTING_THREADS=0` | **In 2.48.5 `0` is VALID** (the parser accepts `*newValue <= 8`, `SkiaPaintingEngine.cpp:233‑236` [VS]) and means **paint on the main thread** (`:56‑57` comment [VS]) — avoids the threaded‑Skia crash. Affects *paint*, not *when* a frame is scheduled. (A web source claiming "0 is out of range / rejected" was reading a different branch — **wrong for 2.48.5** [VS].) | `SkiaPaintingEngine.cpp:53‑57,231‑238` [VS] |
| **CPU rendering enable** | env | `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1` | **IS honored in 2.48.5** — selects the CPU‑only worker pool; with `=1` and `CPU_PAINTING_THREADS=0` → CPU rendering on the main thread (documented at `SkiaPaintingEngine.cpp:53‑57` [VS]). (A web source claiming it's a no‑op in 2.48 was reading a different branch — **wrong for 2.48.5** [VS].) | `SkiaPaintingEngine.cpp:53‑57` [VS] |
| **Prefer ~60 fps updates** | setting | WebCore `Settings::preferPageRenderingUpdatesNear60FPSEnabled()` (feeds `preferredFrameInterval`) | When the nominal display fps is high/odd, rounds toward 60. Not exposed as a public `WebKitSettings` getter; only matters if nominal fps ≠ 60. | `Page.cpp:2577,2601` + `AnimationFrameRate.cpp:60` [VS] |
| **Render‑node device** | env | `WEBKIT_WEB_RENDER_DEVICE_FILE` | Picks the DRM render node for GPU buffer allocation. **Irrelevant** to cadence. | `DRMDevice.cpp:202` [VS] |
| **Show FPS / damage (debug)** | env | `WEBKIT_SHOW_FPS`, `WEBKIT_SHOW_DAMAGE` | On‑screen FPS / damage‑rect overlay (diagnostics only). | `TextureMapperFPSCounter.cpp:40`, `TextureMapperDamageVisualizer.cpp:41` [VS] |
| **`webkit_web_view_get_snapshot()`** | API | **NOT on WPE 2.48.5; added in WPE 2.52** | Our 2.48.5 `WebKitWebView.h` has no `..._get_snapshot`/`..._snapshot_finish` (only the `WebKitSnapshotError` enum in `WebKitError.h`) [VS]. It **landed in 2.52** (commit 303449@main [WEB]); there the WPE return type is **`WebKitImage*`** (not cairo/GdkTexture, which are GTK‑gated), and `webkit_image_as_bytes()` gives **premultiplied BGRA8888** CPU pixels (== `QImage::Format_ARGB32_Premultiplied` LE). **But even in 2.52 it does NOT force a fresh paint** — it returns the *existing* backing store [WEB]. Not usable for us until/unless we bump WebKit. | headers grep [VS]; https://wpewebkit.org/reference/stable/wpe-webkit-2.0/class.WebView.html , https://commits.webkit.org/303449@main [WEB] |
| **`wpe_view_set_target_refresh_rate()`** | API | **DOES NOT EXIST** | No refresh‑rate setter on `WPEView`/`WPEToplevel` in 2.48; the only refresh API is on `WPEScreen` (above). | `WPEView.h`/`WPEToplevel.h` grep [VS] |
| **Force repaint — high‑level glib API** | API | **None on WebKitWebView (WPE)** | No `webkit_web_view_*` maps to force‑repaint on WPE. (`forceRepaint`/`forceRepaintAsync` exist **internally**, `LayerTreeHost.cpp:290‑333` [VS]; there's also a private test‑only `webkitWebViewForceRepaintForTesting()` — not API‑stable [WEB].) Trigger repaint indirectly (DOM mutation / resize / our existing trick). | headers grep [VS] |
| **Force repaint — low‑level WK C API (ships with WPE)** | API | `void WKPageForceRepaint(WKPageRef, void* ctx, WKPageForceRepaintFunction)` (UIProcess) · `void WKBundlePageForceRepaint(WKBundlePageRef)` (injected bundle) | The real "force a frame now" primitive (drives internal `updateRenderingWithForcedRepaint`, renamed in bug 273596). **Caveat:** declared in 2.48.5 source (`WKPage.h:258`, `WKBundlePagePrivate.h:58` [VS]) but the WK C API headers are **NOT installed in our `build/stage`** [VS] — using them means building against WebKit's internal C API and obtaining a `WKPageRef`, heavier/less‑supported than our DOM trick. | decl [VS]; rename https://bugs.webkit.org/show_bug.cgi?id=273596 [WEB] |

> Not‑applicable / wrong‑for‑WPE knobs (don't waste time): `WEBKIT_DISABLE_COMPOSITING_MODE` (WebKitGTK
> legacy; see `wpe-rendering-protocol.md §5`); any `LayerFlushThrottleState` advice (that subsystem was
> removed — `grep` finds **no** `LayerFlushThrottleState` in 2.48 [VS]).

---

## 3. Public WPE API surface for "get a frame" (ground truth from headers)

From `build/stage/usr/include/wpe-webkit-2.0/` [VS]:

- **Consume frames:** connect to the `WPEView` `"buffer-rendered"` signal; read pixels with
  `wpe_buffer_shm_get_data()` / `wpe_buffer_shm_get_stride()` (we do this; correct).
- **`wpe_view_render_buffer(view, buffer, damage_rects, n, **err)`**, **`wpe_view_buffer_rendered(view, buffer)`**,
  **`wpe_view_buffer_released(view, buffer)`** — these are **producer‑side** (platform) calls. **An embedder
  must NOT call them** (double‑free with an embedded `WebKitWebView` — see `wpe-rendering-protocol.md §2`). [VS]
- **No** `render now` / `schedule update` / `damage` / `present` API on `WPEView` or `WPEToplevel`. The full
  `WPEView.h` `WPE_API` list (verified) contains visibility/map/focus/toplevel/screen/cursor/gesture/event
  functions and the three buffer functions above — nothing that forces a paint. [VS]
- **`WPEScreen`** has `get/set_refresh_rate`, `get_width/height`, `get_id`, etc. — but the **headless display
  exposes no screen**, so there is no `WPEScreen` instance to configure. [VS]
- **`webkit_web_view_get_snapshot()`** — **absent** on WPE 2.48.5 (added in **2.52**, returns `WebKitImage*`;
  even there it does not force a paint). [VS][WEB]
- **`WKPageForceRepaint()` / `WKBundlePageForceRepaint()`** (low‑level WK C API) DO exist and ship with WPE
  and *can* force a frame — but the C API headers are **not in our `build/stage`** SDK, so reaching them is a
  heavier integration than our DOM‑mutation trigger (§2, §5 #3). [VS]

---

## 4. The idiomatic "frame on demand" pattern for headless/offscreen WPE

Confirmed against upstream tooling (external sweep, URLs below):

- **There is NO ready‑made "grab one frame" helper for the new WPEPlatform path** in either WebKit or cog;
  **signal‑driven consumption (`buffer-rendered` + read `WPEBufferSHM`) IS the idiomatic pattern, and it is
  exactly what we do.** [WEB]
  - Igalia **`cog`'s headless platform still uses the LEGACY libwpe + WPEBackend‑fdo path** (`export_shm_buffer`
    + `dispatch_frame_complete` on a `max_fps` timer, default 30) — a frame **pacer/sink that never reads
    pixels**, not a model for grabbing frames. https://github.com/Igalia/cog/blob/master/platform/headless/cog-platform-headless.c [WEB]
  - WPE **`MiniBrowser --headless`** does call `wpe_display_headless_new()` (the new path) but **does not
    snapshot or save frames** — it just runs the page.
    https://github.com/WebKit/WebKit/blob/main/Tools/MiniBrowser/wpe/main.cpp [WEB]
  - The canonical offscreen frame‑grabber, **`Tools/wpe/backends/fdo/HeadlessViewBackendFdo.cpp`**, is
    **legacy**: on each `export_shm_buffer` it memcpy's the `wl_shm_buffer` into an `SkImage` and `snapshot()`
    returns the latest — i.e. the same "consume the buffer the signal hands you" model, just on the old API.
    https://github.com/WebKit/WebKit/blob/main/Tools/wpe/backends/fdo/HeadlessViewBackendFdo.cpp [WEB]
- **A bare `scrollBy`/scroll event does NOT reliably commit a frame** — WebKitTestRunner deliberately calls
  `WKBundlePageForceRepaint` *after* sending scroll events, with the comment "Triggers a scrolling tree
  commit." This **independently validates our `scrollBy` + DOM‑mutation trick**: the DOM mutation is what
  forces the commit. https://github.com/WebKit/WebKit/blob/main/Tools/WebKitTestRunner/InjectedBundle/EventSendingController.cpp [WEB]
- **To get a frame, you must cause a rendering update**, then wait for the next vblank tick to deliver it.
  WebKit's own internal "ensure a frame now" primitive is `forceRepaintAsync()` /
  `dispatchAfterEnsuringDrawing()` (`LayerTreeHost.cpp:320‑333`, `DrawingAreaCoordinatedGraphics.cpp:398‑415`
  [VS]) — but those are not exposed publicly on WPE, so an embedder approximates them by **dirtying the page**
  (DOM mutation / `scrollTo` / a forced resize) — which is precisely our current trick and is correct. The
  bottleneck is not *triggering* the update; it's the **vblank clock** that *delivers* it (§1.4).
- `callAfterNextPresentationUpdate()` (the WebKit‑internal "tell me when the next frame is presented") is
  implemented for WPE by hooking the same `buffer-rendered` signal — `WPEWebViewPlatform.cpp:595‑606` [VS] —
  confirming `buffer-rendered` is the canonical "a frame happened" event.

---

## 5. Recommended fixes — ranked for our embedded‑headless‑CPU case

> Constraints honored: we keep an **embedded `WebKitWebView`**; we **never** call
> `wpe_view_buffer_released()`/`_rendered()`; we read **SHM** buffers; we render on **CPU**.

**#0 — First, re‑read the existing `flip-latency` logs in isolation to classify A1 vs A2 (5 minutes, no code).**
[VS] The engine already times the `buffer-rendered` signal itself (`main.cpp:1548‑1550`: `dt` between
buffers, `flip-latency` from swipe→signal). If those *signal* timestamps are ≤~50 ms after a swipe and only
the **panel** lags → **A1** (WPE is fine; it's the e‑ink present). If the *signal* itself is on the ~6 s grid
→ **A2** (a real WPE gate to hunt). **Do this before changing anything** — it picks which of the fixes below
even applies.

**#1 (if A1 — the likely case) — The fix is on the panel‑present side, and is already in the app.** [VS]
`EpaperRefresh` (`main.cpp:2459‑2526`) already drives `EPFramebuffer::swapBuffers` from
`QQuickWindow::frameSwapped` so a page turn presents immediately (fast mono per turn, full colour flash
every Nth). (The `afterRendering` variant self‑deadlocks the render loop's fb mutex and is diagnostic‑only —
`RMWEB_MANUAL_PRESENT`.) The WPE buffer is produced quickly (≤16 ms per the 60 fps timer clock); ensure each
new `QImage`
triggers a present. Tuning lives in `remarkable-eink-refresh.md`, **not here**. No WPE change needed.

**#2 (control / diagnostic) — Run once with the software vblank forced.** [VS]
```sh
export WEBKIT_FORCE_VBLANK_TIMER=1   # read at DisplayVBlankMonitor.cpp:46
```
Expected to be a **no‑op** for the cadence (we resolve to `displayID 0` → already the timer monitor, §1.4),
but running it confirms that and rules world B fully out. If — surprisingly — it *does* change the
`buffer-rendered` cadence, then `displayID != 0` in practice (a DRM display sneaked in as primary) and this
**is** the fix; in that case also consider asserting our headless display as primary via
`wpe_display_set_primary(display)` right after creating it (`WPEDisplay.cpp:240` [VS]).

**#3 (if A2 — unconfirmed gate) — Keep/strengthen the on‑demand repaint trigger.** [VS] Our
`scrollBy + 1‑char DOM mutation` (`main.cpp:930‑953`) is the correct public way to force a frame on WPE (no
public glib `forceRepaint`, no `get_snapshot`) — and it is **independently validated**: WebKitTestRunner uses
`WKBundlePageForceRepaint` to "trigger a scrolling tree commit" after scroll events, i.e. a bare scroll does
not reliably commit, the mutation does [WEB]. If A2 is real, make delivery robust by toggling a trivial
attribute on `document.documentElement` for ~3 frames after a swipe (successive updates) and verify the
`buffer-rendered` deltas drop. **Stronger (but heavier) alternative:** call **`WKPageForceRepaint(WKPageRef…)`**
(ships with WPE; drives `updateRenderingWithForcedRepaint`) — gated by the caveat that the WK C API headers
are not in our `build/stage` (§2), so it requires building against WebKit's internal C API and obtaining a
`WKPageRef` from the `WebKitWebView`. Belt‑and‑suspenders; only if #0 shows the *signal* itself is gated.

**#4 — (Heavier, only if we leave the headless display) Implement a minimal `WPEScreen` reporting 60 Hz.**
[INF] A custom `WPEDisplay` exposing one `WPEScreen` with `wpe_screen_set_refresh_rate(screen, 60000)`
(milli‑Hz → 60 Hz) gives a real nominal fps and a defined DisplayLink rate, instead of the `displayID 0`
fallback. Worth it only if we replace `wpe_display_headless_new()` for other reasons. (`WPEScreen.h:70‑72`,
units `WPEScreen.cpp:252` [VS].)

**Explicitly rejected:** `webkit_web_view_get_snapshot()` (absent on WPE [VS]);
`wpe_view_set_target_refresh_rate()` (absent [VS]); calling `wpe_view_render_buffer`/`buffer_rendered`
ourselves (producer‑side; double‑free [VS]); any `LayerFlushThrottleState` tweak (subsystem removed from 2.48
[VS]).

---

## 6. Device probes (confirm the source‑level conclusion, classify A1 vs A2)

The source already says `displayID == 0` → 60 fps timer (§1.4); these probes *confirm* it and split A1/A2.

**Probe 1 — confirm displayID 0 (one log line).** In `WpeEngine::start()` after creating the view:
```c
WPEDisplay *prim = wpe_display_get_primary();
g_message("primary=%p ours=%p n_screens=%u view_screen=%p",
          prim, display, wpe_display_get_n_screens(prim), wpe_view_get_screen(wpeView));
```
Expect `primary == ours`, `n_screens == 0`, `view_screen == NULL` → `displayID 0` → timer monitor. If so,
`WEBKIT_FORCE_VBLANK_TIMER` is a no‑op (world A confirmed). If `n_screens != 0` or `view_screen != NULL`,
re‑open the world‑B path in §5 #2.

**Probe 2 — classify A1 (panel slow) vs A2 (WPE signal slow): read the existing logs.** The engine already
logs `dt` (inter‑`buffer-rendered` interval) and `flip-latency` (swipe→signal) at `main.cpp:1548‑1550`. With NO
input, look at `dt`: if `buffer-rendered` is *not* firing every ~6 s when idle (few/no frames) → the "~6 s"
was the panel, **A1**. If `dt ≈ 6000 ms` with zero input → **A2** (a real WPE gate; the signal itself is
periodic). After a swipe, `flip-latency` ≤ ~50 ms ⇒ A1; ≈ several seconds ⇒ A2.

**Probe 3 — the control.** One run with `WEBKIT_FORCE_VBLANK_TIMER=1`; compare `dt`/`flip-latency`. No change
⇒ world A nailed.

**Probe 4 — "is a frame even produced?" (diagnostic env, all read in 2.48.5 source).** [VS]
- `WEBKIT_SHOW_DAMAGE=1` — overlays the Skia damage rects: confirms whether a scroll/DOM mutation produces a
  repaint region at all (`TextureMapperDamageVisualizer.cpp:41`).
- `WEBKIT_SHOW_FPS=<seconds>` — FPS overlay; the value is the averaging interval in seconds
  (`TextureMapperFPSCounter.cpp:40‑42`).
- `WEBKIT_SHOW_COMPOSITING_DEBUG_VISUALS=1` — repaint counter + layer borders (`WebKitSettings.cpp:339`).
- `WEBKIT_DEBUG='Scrolling,Compositing,Layers'` + `DisplayLink` — log channels (also enable the `DisplayLink`
  channel to see the start/stop/`fired` lines from §1.4). [WEB: https://docs.webkit.org/Build%20&%20Debug/Logging.html]

---

## 7. Deltas to fold back into other docs

- `remarkable-eink-refresh.md` says "There is also a ~6 s **periodic** WPE re‑render even with no input."
  **Refine:** there is **no intrinsic WebKit idle heartbeat** in 2.48 (no 6 s constant in the render path
  [VS]), and the WPE render clock here is a **60 fps software timer** (we resolve to `displayID 0`, proven in
  §1.4 [VS]). So the ~6 s is **not** WPE producing buffers slowly — it is the **epaper/QSG panel‑present loop**
  (A1, the same one that doc analyses) unless Probe 2 (§6) shows the `buffer-rendered` *signal itself* on a
  6 s grid (A2, an unconfirmed gate to hunt). `WEBKIT_FORCE_VBLANK_TIMER=1` is expected to be a no‑op control.
- `wpe-rendering-protocol.md`: add `WEBKIT_FORCE_VBLANK_TIMER` to the env‑var list and note that on WPE the
  only "force a frame" path is to dirty the page (no public `forceRepaint`, no `get_snapshot`).
- `device-profile.md`: record that this WPE build has `HAVE(DISPLAY_LINK)=1` and that headless → no `WPEScreen`
  → `displayID` resolves via `ScreenManager::primaryDisplayID()`.

---

## 8. Source index (key file:line, all [VS] in the local 2.48.5 tree)

- Headless view 60 fps frame timer & signal emit: `Source/WebKit/WPEPlatform/wpe/headless/WPEViewHeadless.cpp:46‑130`.
- Headless toplevel constructed ACTIVE: `…/headless/WPEToplevelHeadless.cpp:39‑44`.
- Headless display has no screen, attaches toplevel: `…/headless/WPEDisplayHeadless.cpp:87‑93`.
- Activity‑state init for the embedded view: `Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp:67‑82, 595‑606`.
- `isVisibleAndActive`: `Source/WebCore/page/Page.cpp:3206`; throttle reasons/intervals:
  `Source/WebCore/platform/graphics/AnimationFrameRate.{h:33‑56,cpp:47‑86}`; preferred interval:
  `Page.cpp:2577,2601`.
- UIProcess buffer chain (no timer): `Source/WebKit/UIProcess/wpe/AcceleratedBackingStoreDMABuf.cpp:55,133‑180`.
- WebProcess producer (on‑demand flush): `…/CoordinatedGraphics/LayerTreeHost.cpp:150‑167,175‑258,290‑333,494‑524`;
  `…/DrawingAreaCoordinatedGraphics.cpp:218,317‑326,398‑415`.
- DisplayLink / vblank: `Source/WTF/wtf/PlatformHave.h:350‑352`;
  `Source/WebKit/WebProcess/WebPage/WebDisplayRefreshMonitor.cpp:78‑101`;
  `Source/WebKit/UIProcess/DisplayLink.cpp:42,185‑227`;
  `Source/WebKit/UIProcess/glib/DisplayVBlankMonitor.cpp:44‑58`,
  `…/DisplayVBlankMonitorTimer.cpp:40‑49`, `…/DisplayVBlankMonitorDRM.cpp:60‑179`.
- Primary display = first‑created `WPEDisplay` (→ our headless, no screens): `Source/WebKit/WPEPlatform/wpe/WPEDisplay.cpp:85‑90,216‑243`.
- WPE primary‑display id resolution (no screens → id 0): `Source/WebKit/UIProcess/wpe/ScreenManagerWPE.cpp:42‑99`.
- Display platform extension priorities (drm/wayland/headless): `…/drm/WPEDisplayDRM.cpp:70`, `…/wayland/WPEDisplayWayland.cpp:97`, `…/headless/WPEDisplayHeadless.cpp:69`.
- Env vars: `getenv("WEBKIT_FORCE_VBLANK_TIMER")` `DisplayVBlankMonitor.cpp:46`;
  `getenv("WEBKIT_DISABLE_ASYNC_SCROLLING")` `DrawingAreaCoordinatedGraphics.cpp:218`;
  Skia `SkiaPaintingEngine.cpp:53‑57,231‑238` (`WEBKIT_SKIA_ENABLE_CPU_RENDERING`,
  `WEBKIT_SKIA_CPU_PAINTING_THREADS` — `0` is valid = main thread);
  `getenv("WEBKIT_WEB_RENDER_DEVICE_FILE")` `Source/WebKit/UIProcess/glib/DRMDevice.cpp:202`;
  diagnostics `TextureMapperDamageVisualizer.cpp:41`, `TextureMapperFPSCounter.cpp:40`, `WebKitSettings.cpp:339`.
- Force‑repaint C API (ships, headers not staged): `Source/WebKit/UIProcess/API/C/WKPage.h:258`,
  `…/WebProcess/InjectedBundle/API/c/WKBundlePagePrivate.h:58`.
- Public API surface (no snapshot / no target‑refresh): headers under
  `build/stage/usr/include/wpe-webkit-2.0/` (`WPEView.h`, `WPEScreen.h`, `wpe/WebKitWebView.h`, `wpe/WebKitError.h`).

**External sources [WEB]:** snapshot landed 2.52 — https://commits.webkit.org/303449@main ,
https://wpewebkit.org/reference/stable/wpe-webkit-2.0/class.WebView.html (absent in 1.1:
https://wpewebkit.org/reference/stable/wpe-webkit-1.1/class.WebView.html );
`forceRepaint`→`updateRenderingWithForcedRepaint` rename — https://bugs.webkit.org/show_bug.cgi?id=273596 ;
`WKPageForceRepaint`/`WKBundlePageForceRepaint` — github.com/WebKit/WebKit `Source/WebKit/UIProcess/API/C/WKPage.h`,
`…/WebProcess/InjectedBundle/API/c/WKBundlePagePrivate.h` ; cog headless still legacy fdo —
https://github.com/Igalia/cog/blob/master/platform/headless/cog-platform-headless.c ;
legacy offscreen grabber — https://github.com/WebKit/WebKit/blob/main/Tools/wpe/backends/fdo/HeadlessViewBackendFdo.cpp ;
scroll needs forced commit — https://github.com/WebKit/WebKit/blob/main/Tools/WebKitTestRunner/InjectedBundle/EventSendingController.cpp ;
WPE refresh‑rate units mHz + screen API — https://wpewebkit.org/release/wpewebkit-2.47.1.html ;
env var / logging reference — https://docs.webkit.org/Build%20&%20Debug/Logging.html .
**Note:** a web sweep also claimed `WEBKIT_SKIA_CPU_PAINTING_THREADS=0` is "out of range/rejected" and
`WEBKIT_SKIA_ENABLE_CPU_RENDERING` is a no‑op in 2.48 — **both are wrong for 2.48.5** per local source
(`SkiaPaintingEngine.cpp:53‑57,235`): `0` is accepted (= main‑thread CPU) and the enable flag is honored.
