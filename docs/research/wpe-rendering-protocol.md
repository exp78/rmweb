# WPE WebKit 2.48 — WPEPlatform rendering protocol, buffer lifecycle & frame loop

**Scope.** How to *correctly* drive rendering when you embed a high‑level `WebKitWebView` on a
custom/headless `WPEDisplay` using the **new WPEPlatform API** (`wpe-platform-2.0`, WPE WebKit 2.48),
on a CPU‑only device (Mesa softpipe/llvmpipe, surfaceless EGL, no GPU). Written for the rmweb engine
(`engine/wpeqt/main.cpp`).

**Authority.** Every claim below is checked against the **actual 2.48.5 source tree** unpacked locally at
`build/src/wpewebkit-2.48.5 (repo-local, gitignored)` (file:line citations are to that
tree, which matches upstream `wpewebkit-2.48.5`). Where a claim comes from the web it is marked and linked.
Skeptical note: some widely‑repeated WebKitGTK advice (e.g. `WEBKIT_DISABLE_COMPOSITING_MODE`) is **wrong for
WPE 2.48** — see §5.

---

## 0. TL;DR — the three fixes

| # | Symptom | Root cause (verified) | Fix |
|---|---------|----------------------|-----|
| 1 | Frames stop after the first ~2; no repaint on DOM mutation/scroll | WebKit **suspends painting unless `ActivityState::IsVisible`**, and `IsVisible` is driven by `wpe_view_get_mapped()` — not `set_visible`. A fresh headless view comes up **unmapped** with a **0×0 toplevel**; if you show it while it's 0×0 or the map transition doesn't deliver, the page never renders steadily. | **Size the toplevel first** (`wpe_toplevel_resize` to e.g. 1620×2160), then force `visible=FALSE→TRUE`, then **assert `wpe_view_get_mapped()==TRUE` and width/height!=0**. (§3) |
| 2 | `wpe_view_buffer_released()` SEGFAULTs (double‑free) | When embedding `WebKitWebView`, **two** owners already release each buffer: WebKit's `AcceleratedBackingStoreDMABuf` (UIProcess) and the headless `WPEView`'s own frame timer. Your call is a 3rd release → double/triple free. | **Never call `wpe_view_buffer_rendered()` / `wpe_view_buffer_released()`** from an embedder. They are producer‑side (platform) calls. Just copy pixels out of `buffer-rendered` and return. (§2) |
| 3 | Rendering at `scrollY>0` SEGFAULTs on software GL; `scrollY==0` is fine | Scrolling produces a large multi‑region repaint that goes through WPE 2.48's **threaded Skia rendering** path; that path had thread‑safety crashes (`NEWS` 2.48.2/2.48.3) and the compositor still uses EGL even with CPU tiles. | Force **single‑threaded CPU painting**: `WEBKIT_SKIA_CPU_PAINTING_THREADS=0` (+ `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`). Keeps repaint off the WorkerPool. Your content‑shift workaround can then be dropped. (§4, §5) |

---

## 1. The cast: who owns and drives what

When you do this (your `main.cpp:88‑104`):

```c
WPEDisplay *display = wpe_display_headless_new();          // WPEDisplayHeadless
wpe_display_connect(display, &err);
m_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, nullptr));
WPEView *wpeView = webkit_web_view_get_wpe_view(m_view);    // a WPEViewHeadless
g_signal_connect(wpeView, "buffer-rendered", ...);
```

…the following objects are wired up **for you**, inside `g_object_new(WEBKIT_TYPE_WEB_VIEW…)`:

- Because the build has `ENABLE(WPE_PLATFORM)` and you passed a `"display"`, `WebKitWebView` creates a
  **`WKWPE::ViewPlatform`**, *not* the legacy libwpe view
  — `Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp:852‑854`:
  ```cpp
  #if ENABLE(WPE_PLATFORM)
      webView->priv->view = WKWPE::ViewPlatform::create(webkit_web_view_get_display(webView), configuration.get());
  ```
- `ViewPlatform`'s constructor creates the `WPEView` (`wpe_view_new(display)` → a **`WPEViewHeadless`**),
  wires its activity state, and creates the backing store
  — `Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp:58‑62, 130‑132`:
  ```cpp
  ViewPlatform::ViewPlatform(...) : m_wpeView(adoptGRef(wpe_view_new(display))) { ...
      m_backingStore = AcceleratedBackingStoreDMABuf::create(*m_pageProxy, m_wpeView.get());
  ```
- The WebProcess side uses **`AcceleratedSurfaceDMABuf`** to ship composited frames
  — `Source/WebKit/WebProcess/WebPage/AcceleratedSurface.cpp:54‑61`. (On a no‑GPU box it falls back to a
  **SharedMemory** transport — see §5 — but the class is still "DMABuf".)

So **`webkit_web_view_get_wpe_view()` returns the platform's presenter view**, and WebKit's own
`AcceleratedBackingStoreDMABuf` is *already connected* to that view's `buffer-rendered` / `buffer-released`
signals. Your `g_signal_connect` is an **additional** observer, not a replacement.

### Role table (new WPEPlatform API)

| Role | Object | Responsibility |
|------|--------|----------------|
| **Producer of pixels** | WebProcess (Skia) → `AcceleratedSurfaceDMABuf` | renders the page, allocates the frame buffer, sends it to UIProcess |
| **Submitter** | UIProcess `AcceleratedBackingStoreDMABuf` | calls `wpe_view_render_buffer(view, buffer, damage…)` |
| **Presenter / "the view"** | the `WPEView` subclass (here `WPEViewHeadless`; for a real panel you'd write your own) | implements `render_buffer`; **emits `buffer-rendered` + `buffer-released`** |
| **Frame‑done consumer** | UIProcess `AcceleratedBackingStoreDMABuf` (again) | receives `buffer-rendered` → `FrameDone` IPC → unblocks next paint; receives `buffer-released` → `ReleaseBuffer` IPC |
| **You (embedder)** | your `buffer-rendered` handler | **read‑only**: copy the pixels out, display them. Release nothing. |

The single most important sentence: **in the new API the `WPEView` *subclass* is the producer of
`buffer-rendered`/`buffer-released`; the embedding application is a passive consumer.** This is the opposite
of the old `WPEBackend-fdo` "export buffer → you release it" model.

---

## 2. The exact contract of the four functions (quoted from source)

### `wpe_view_render_buffer()` — submitter → presenter
`Source/WebKit/WPEPlatform/wpe/WPEView.cpp:878‑900`:
```c
/**
 * wpe_view_render_buffer:
 * ...
 * Render the given @buffer into @view.
 * If this function returns %TRUE you must call wpe_view_buffer_rendered() when the buffer
 * is rendered and wpe_view_buffer_released() when it's no longer used by the view.
 *
 * Returns: %TRUE if buffer will be rendered, or %FALSE otherwise
 */
gboolean wpe_view_render_buffer(WPEView* view, WPEBuffer* buffer, const WPERectangle* damageRects,
                                guint nDamageRects, GError** error)
{
    ...
    auto* viewClass = WPE_VIEW_GET_CLASS(view);
    return viewClass->render_buffer(view, buffer, damageRects, nDamageRects, error);   // -> vfunc
}
```
**Who calls it:** WebKit's UIProcess (`AcceleratedBackingStoreDMABuf::renderPendingBuffer`,
`Source/WebKit/UIProcess/wpe/AcceleratedBackingStoreDMABuf.cpp:152‑167`). **You never call this.** It is a
*vfunc dispatcher* — the actual work is `WPEViewClass::render_buffer` (declared
`Source/WebKit/WPEPlatform/wpe/WPEView.h:58‑62`).

The doc sentence *"you must call `wpe_view_buffer_rendered()` … and `wpe_view_buffer_released()`"* is addressed
to **the implementer of `render_buffer`** (the platform view), **not** to the app that called
`wpe_view_render_buffer`.

### `wpe_view_buffer_rendered()` — presenter → WebKit ("frame done")
`Source/WebKit/WPEPlatform/wpe/WPEView.cpp:901‑913`:
```c
/** Emit #WPEView::buffer-rendered signal to notify that @buffer has been rendered. */
void wpe_view_buffer_rendered(WPEView* view, WPEBuffer* buffer)
{ ...; g_signal_emit(view, signals[BUFFER_RENDERED], 0, buffer); }
```
Signal doc, `:366‑371`: *"Emitted to notify that the buffer has been rendered in the view."*
**This is the frame‑complete handshake.** WebKit's handler turns it into `FrameDone`:
`AcceleratedBackingStoreDMABuf.cpp:177‑181`:
```c
void AcceleratedBackingStoreDMABuf::bufferRendered()
{
    frameDone();                              // -> Messages::AcceleratedSurfaceDMABuf::FrameDone()  (line 171‑175)
    m_committedBuffer = WTFMove(m_pendingBuffer);
}
```
There is **no separate `wpe_view_…_frame_complete` symbol** in the new API — `buffer-rendered` *is* it.
(The old FDO API had a distinct `wpe_view_backend_exportable_fdo_dispatch_frame_complete()`.)

### `wpe_view_buffer_released()` — presenter → WebKit ("buffer reusable")
`Source/WebKit/WPEPlatform/wpe/WPEView.cpp:916‑928`:
```c
/** Emit #WPEView::buffer-released signal to notify that @buffer is no longer used and
 *  can be destroyed or reused. */
void wpe_view_buffer_released(WPEView* view, WPEBuffer* buffer)
{ ...; g_signal_emit(view, signals[BUFFER_RELEASED], 0, buffer); }
```
Signal doc, `:382‑388`: *"Emitted to notify that the buffer is no longer used by the view and can be destroyed
or reused."* WebKit's handler returns the buffer to the WebProcess pool:
`AcceleratedBackingStoreDMABuf.cpp:183‑193` → `Messages::AcceleratedSurfaceDMABuf::ReleaseBuffer(id, fence)`.

### `buffer-rendered` (the signal you connect to)
`g_signal_new("buffer-rendered", … G_TYPE_NONE, 1, WPE_TYPE_BUFFER)` —
`Source/WebKit/WPEPlatform/wpe/WPEView.cpp:372‑379`. It is a **plain notification**. It does **not** transfer
ownership and does **not** obligate you to release anything. (Releasing is the *presenter's* job, and for a
headless WebKitWebView the presenter is `WPEViewHeadless`, which already does it — next section.)

---

## 3. What actually drives continuous rendering (the frame loop), and why it stalls

### 3.1 The headless self‑clocking loop
`WPEViewHeadless` (the presenter you got) implements `render_buffer` by **stashing the buffer and arming a
~60 fps one‑shot timer**, and on each tick it releases the previous buffer and emits `buffer-rendered`.
Full verbatim, `Source/WebKit/WPEPlatform/wpe/headless/WPEViewHeadless.cpp`:

```c
// frame timer callback (set up in ...Constructed, lines 87‑99):
g_source_set_callback(priv->frameSource.get(), [](gpointer userData) -> gboolean {
    auto* view = WPE_VIEW(userData);
    auto* priv = WPE_VIEW_HEADLESS(view)->priv;
    if (priv->committedBuffer)
        wpe_view_buffer_released(view, priv->committedBuffer.get());   // (1) return previous buffer
    priv->committedBuffer = WTFMove(priv->pendingBuffer);              // (2) promote
    wpe_view_buffer_rendered(view, priv->committedBuffer.get());       // (3) frame‑done -> next paint
    ...
}, object, nullptr);

// render_buffer vfunc (lines 115‑130):
static gboolean wpeViewHeadlessRenderBuffer(WPEView* view, WPEBuffer* buffer, const WPERectangle*, guint, GError**)
{
    auto* priv = WPE_VIEW_HEADLESS(view)->priv;
    priv->pendingBuffer = buffer;
    auto now = g_get_monotonic_time();
    auto next = priv->lastFrameTime + (G_USEC_PER_SEC / 60);
    priv->lastFrameTime = now;
    g_source_set_ready_time(priv->frameSource.get(), next <= now ? 0 : next);
    return TRUE;
}
```
Class doc, `:34‑42`: it *"does not display buffers on any native surface; instead it notifies that each buffer
has been rendered, **driving the rendering loop** without producing visible output."*

The full cycle, end to end:
```
WebProcess paints  ─Submit IPC→  AcceleratedBackingStoreDMABuf.renderPendingBuffer()
   → wpe_view_render_buffer()  → WPEViewHeadless.render_buffer() arms 60Hz timer  → returns TRUE
   ── timer fires ──→  wpe_view_buffer_released(prev) + wpe_view_buffer_rendered(cur)
   →  bufferRendered() → frameDone() → FrameDone IPC  →  WebProcess may paint the NEXT frame
```
**Conclusion:** the loop is *self‑pumping*, **but only while WebKit keeps producing paints.** The timer does
**not** free‑run; it only ticks once per submitted buffer. So "2 frames then nothing" means **WebKit stopped
producing paints**, not that the timer died.

### 3.2 Why WebKit stops: the view is not mapped → painting suspended
WebKit derives `ActivityState::IsVisible` **from `wpe_view_get_mapped()`**, not from `set_visible`:
`Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp:67‑68` (at construction) and `84‑87` (on change):
```cpp
if (wpe_view_get_mapped(m_wpeView.get()))
    m_viewStateFlags.add(WebCore::ActivityState::IsVisible);
...
g_signal_connect(m_wpeView.get(), "notify::mapped", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer ud) {
    auto& webView = *reinterpret_cast<ViewPlatform*>(ud);
    webView.activityStateChanged(WebCore::ActivityState::IsVisible, wpe_view_get_mapped(view));
}), this);
```
And the WebProcess **suspends painting when not visible**:
`Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/DrawingAreaCoordinatedGraphics.cpp:64`:
```cpp
, m_isPaintingSuspended(!(parameters.activityState & ActivityState::IsVisible))
```
(painting only resumes when `IsVisible` flips, `:341`). Corroborated by upstream `NEWS` (2.47.3):
*"Pause rendering when current toplevel window is in suspended state."*

**The exact construction trace for your headless `WebKitWebView`** (verified call‑by‑call):

1. `ViewPlatform` does `wpe_view_new(display)` (`WPEWebViewPlatform.cpp:58`). `wpe_view_new` →
   `wpeDisplayCreateView` → `WPEDisplayHeadless::create_view`, which **attaches a `WPEToplevelHeadless`**
   (state `ACTIVE`) via `wpe_view_set_toplevel` (`WPEDisplayHeadless.cpp:88‑93`). That fires the headless
   view's `notify::toplevel` handler (`WPEViewHeadless.cpp:67‑81`), which calls `wpe_view_map(view)` —
   **but `priv->visible` is still `FALSE`** (GObject zero‑inits the private struct; nothing set it), so
   `wpe_view_map` returns at guard (B) below. **View is NOT mapped.** Also note the toplevel default size is
   **0×0** (`WPEToplevel` base never initializes `priv->width/height`, `WPEToplevel.cpp:287‑288`), so the
   handler's `if (width && height) wpe_view_resized(...)` is skipped — the page viewport is 0×0.
2. `ViewPlatform` reads `wpe_view_get_mapped()` → **FALSE** → `ActivityState::IsVisible` is **not** in the
   initial state (`WPEWebViewPlatform.cpp:67‑68`). It *does* set `IsInWindow`/`WindowIsActive` (toplevel is
   ACTIVE) and connects `notify::mapped` (`:84‑87`).
3. Your code then resizes and shows. The guards:
   `Source/WebKit/WPEPlatform/wpe/WPEView.cpp:686‑698` and `:731‑744`:
   ```c
   void wpe_view_set_visible(WPEView* view, gboolean visible) {
       if (view->priv->visible == visible) return;          // (A) no‑op if already in that state
       view->priv->visible = visible;
       if (view->priv->visible) wpe_view_map(view); else wpe_view_unmap(view);
       g_object_notify_by_pspec(...PROP_VISIBLE);
   }
   void wpe_view_map(WPEView* view) {
       if (view->priv->mapped || !view->priv->visible) return;                       // (B)
       auto* viewClass = WPE_VIEW_GET_CLASS(view);
       if (viewClass->can_be_mapped && !viewClass->can_be_mapped(view)) return;      // (C: headless has none)
       view->priv->mapped = TRUE;
       g_object_notify_by_pspec(...PROP_MAPPED);            // <-- this is what fires IsVisible
   }
   ```
   Your `wpe_view_set_visible(TRUE)` (`main.cpp:104`) takes `FALSE→TRUE`, so it *does* reach `wpe_view_map`,
   which now passes (visible TRUE, not yet mapped, no `can_be_mapped`) → `mapped = TRUE` → `notify::mapped`
   → `IsVisible` delivered. **So in principle your single call maps it.** The remaining failure modes are
   therefore **ordering / zero viewport**, not "can't map":
   - If `set_visible(TRUE)` is reached while the toplevel is still **0×0** (you only resize `if (top)`, and
     the order in `main.cpp:98‑104` resizes first — good — but verify `top` was non‑NULL), the page lays out
     at 0×0 and most paints are empty/no‑ops → looks like "no frames."
   - If anything elsewhere calls `set_visible(TRUE)` a second time or the view was already visible, guard (A)
     makes it a silent no‑op and no `notify::mapped` fires.

**Robust fix — resize to a real size *before* showing, force an unambiguous transition, and verify:**
```c
WPEView *wpeView = webkit_web_view_get_wpe_view(m_view);

// 1) Size the toplevel FIRST (headless default is 0x0 -> empty paints). This also pushes wpe_view_resized.
WPEToplevel *top = wpe_view_get_toplevel(wpeView);
g_assert(top);                              // headless always attaches one; assert it
wpe_toplevel_resize(top, m_w, m_h);         // e.g. 1620x2160
wpe_view_resized(wpeView, m_w, m_h);        // belt-and-suspenders

// 2) Force a real FALSE->TRUE map transition so notify::mapped fires exactly once.
wpe_view_set_visible(wpeView, FALSE);
wpe_view_set_visible(wpeView, TRUE);        // FALSE->TRUE => map() => notify::mapped => IsVisible

// 3) VERIFY — do not trust it:
g_assert(wpe_view_get_mapped(wpeView));     // must be TRUE, else WebProcess painting stays suspended
g_assert(wpe_view_get_width(wpeView) == m_w && wpe_view_get_height(wpeView) == m_h);
```

> Net: the **mechanism** behind Problem 1 is certain — painting is suspended unless `IsVisible`
> (`DrawingAreaCoordinatedGraphics.cpp:64`), and `IsVisible` tracks `wpe_view_get_mapped()`
> (`WPEWebViewPlatform.cpp:67‑68`). The most likely concrete trigger in your code is a **0×0 viewport at the
> moment of show** (default headless toplevel size) and/or a map transition that didn't deliver. Sizing before
> show + asserting `mapped==TRUE` and a non‑zero size eliminates both. After this, a DOM mutation *and* a real
> scroll should each produce frames continuously.

Secondary suspect (rule it out): the headless frame timer is attached to
`g_main_context_get_thread_default()` **at the time the `WPEView` is constructed**
(`WPEViewHeadless.cpp:99`). You construct the `WebKitWebView` *after*
`g_main_context_push_thread_default(m_ctx)` (`main.cpp:84,96`), so the timer lives on your worker context —
correct. Just make sure that context's `GMainLoop` keeps running (it does, `main.cpp:114`) and that nothing
on it blocks for long inside `buffer-rendered` (your deep‑copy is fine).

---

## 4. Problem 3 — scrolled repaint crashes on software GL

### What changed in 2.48
WPE 2.48 moved tile painting to a **threaded Skia** implementation, and it was crashy at first
(`build/src/wpewebkit-2.48.5/NEWS`):
- 2.48.2: *"Change threaded rendering implementation to use Skia API instead of WebCore display lists that
  were **not thread safe**."*
- 2.48.3: *"Fix a crash introduced by the new threaded rendering implementation using Skia API"* and
  *"…recording layers once and replaying every dirty region in different worker threads."*
- 2.47.4 / fixed in 2.48.5: *"Fix a crash when enabling Skia CPU rendering"* (the EGL‑fence assertion,
  WebKit Bugzilla 286566 / PR 39585 — present in your 2.48.5, so that specific one is patched).

A scroll to `scrollY>0` forces a **large, multi‑region dirty repaint** that is exactly what gets sliced across
the CPU **WorkerPool** and replayed per dirty region. `scrollY==0` after a fresh load is typically a single
clean paint and avoids the WorkerPool stress. On llvmpipe/softpipe this is the fragile path.

### The mitigation: take the WorkerPool out of the equation
`WEBKIT_SKIA_CPU_PAINTING_THREADS=0` makes WebKit paint **synchronously on the main thread** (no worker pool):
`Source/WebCore/platform/graphics/skia/SkiaPaintingEngine.cpp:67‑68`:
```cpp
} else if (numberOfCPUThreads)
    m_cpuWorkerPool = WorkerPool::create("SkiaCPUWorker"_s, numberOfCPUThreads);   // skipped when 0
```
(`numberOfCPUPaintingThreads()` reads the env var, `:233‑236`; default = `cores/2`, capped 8, `:231`.)
With `0`, the threaded replay that the 2.48.2/2.48.3 fixes targeted is never exercised — the safest profile
for a no‑GPU e‑ink reader, at the cost of paint throughput (acceptable for paged reading).

> Caveat: be on the **latest 2.48.x** you can build. 2.48.4 also lists *"Fix several crashes and rendering
> issues"* and *"Fall back to using libdrm to detect device nodes … in the WPEPlatform headless and Wayland
> backends"* — both relevant to a headless/no‑GPU setup. You are on 2.48.5 (good); just don't downgrade.

If, with `CPU_PAINTING_THREADS=0`, scrolled rendering is stable, you can **delete the content‑shift hack**
(`main.cpp:143‑147`) and scroll the page normally — that hack was compensating for this crash.

---

## 5. Forcing CPU / software rendering in 2.48 — the real env‑var map

Two myth‑busts first, both verified by grepping the 2.48.5 tree:

1. **`hardware-acceleration-policy` is NOT the WPE knob.** The property/setter still *exist* but are wrapped in
   `#if PLATFORM(GTK)` and are **compiled out on WPE**
   (`Source/WebKit/UIProcess/API/glib/WebKitSettings.cpp:1561, 3873`; enum `WebKitSettings.h.in:62‑92`).
   On WPE it never did anything; the env var below is the lever.
2. **`WEBKIT_DISABLE_COMPOSITING_MODE` / `WEBKIT_FORCE_COMPOSITING_MODE` DO NOT EXIST in 2.48.5.**
   `grep -rn 'WEBKIT_.*COMPOSITING_MODE' Source/` → **0 hits**. These are stale WebKitGTK‑trac variables.
   Do **not** rely on them. Furthermore you **cannot** turn compositing off at all: if
   `acceleratedCompositingEnabled` is false, WebKit force‑re‑enables it and logs
   *"WebKit cannot function in this mode; changing setting to true"*
   (`Source/WebKit/WebProcess/WebPage/WebPage.cpp:4744‑4746`). The compositor is always on; the only choice is
   whether its tiles are painted on CPU.

The one lever that matters on WPE:

### `WEBKIT_SKIA_ENABLE_CPU_RENDERING`
`Source/WebKit/WebProcess/glib/WebProcessGLib.cpp:175‑185`:
```cpp
#if USE(SKIA)
#if PLATFORM(WPE)
    bool useAcceleratedBuffers = false;     // WPE default = CPU/software
#else
    bool useAcceleratedBuffers = true;      // GTK default = GPU
#endif
    if (const auto e = StringView::fromLatin1(g_getenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING")).trim(...))
        useAcceleratedBuffers = (e == "0"_s);   // "0" => GPU ; anything else => CPU
    ProcessCapabilities::setCanUseAcceleratedBuffers(useAcceleratedBuffers);
#endif
```
On WPE the default is **already CPU**. Set `=1` to be explicit/defensive. When false, no Skia‑GL context is
made and `numberOfGPUPaintingThreads()` early‑returns 0 (`SkiaPaintingEngine.cpp:251‑253`).

### Full table of GL / compositing / DMABuf env vars actually read in 2.48.5

| Env var | Read at (file:line) | Accepts | Effect | Use on no‑GPU? |
|---|---|---|---|---|
| **`WEBKIT_SKIA_ENABLE_CPU_RENDERING`** | `WebProcess/glib/WebProcessGLib.cpp:182` | `"0"`⇒GPU; else⇒CPU (WPE default CPU) | Sets `canUseAcceleratedBuffers`; CPU tile paint, no Skia‑GL | **Set `=1`** (explicit) |
| **`WEBKIT_SKIA_CPU_PAINTING_THREADS`** | `WebCore/.../skia/SkiaPaintingEngine.cpp:233` | int `0..8` | `0`⇒**main‑thread** paint (no WorkerPool); else N workers | **Set `=0`** to dodge Problem 3 crash |
| `WEBKIT_SKIA_GPU_PAINTING_THREADS` | `SkiaPaintingEngine.cpp:258` | int `0..4` | GPU paint threads; **no‑op in CPU mode** (`:252`) | irrelevant |
| `WEBKIT_SKIA_MSAA_SAMPLE_COUNT` | `WebCore/.../skia/PlatformDisplaySkia.cpp:95` | int | MSAA for Skia **GL** context | no‑op in CPU mode |
| **`LIBGL_ALWAYS_SOFTWARE`** | `WPEPlatform/wpe/WPEDisplay.cpp:509` (`isSotfwareRast()`) | `1/y/yes/t/true` | WPE‑Platform reports **no DRM device / no render node** (`:534,:557`) ⇒ surfaceless/software, never opens a GPU node; also the Mesa switch to llvmpipe/softpipe | **Set `=1`** (you do) |
| `WEBKIT_DMABUF_RENDERER_DISABLE_GBM` | `WebProcess/glib/WebProcessGLib.cpp:145` **(`#if PLATFORM(GTK)`)**; report‑only on WPE at `WebKitProtocolHandler.cpp:562` | unset⇒GBM; set⇒off | **Not effective on WPE** (GTK‑gated) | ignore |
| `WPE_DRM_DEVICE` / `WPE_DRM_RENDER_NODE` | `WPEDisplay.cpp:537 / :560` | path | DRM node for WPE‑Platform DRM backend; **bypassed** when `LIBGL_ALWAYS_SOFTWARE` | N/A (headless) |
| `WPE_DRM_DISABLE_ATOMIC` / `WPE_DRM_SCALE` | `WPEPlatform/wpe/drm/WPEDisplayDRM.cpp:158 / :326` | set / num | WPE‑Platform DRM backend only | N/A |
| `WPE_USE_EXPLICIT_SYNC` | `WPEDisplay.cpp:581` | `"0"`⇒off | disables explicit GPU fence sync | only matters with real GPU |
| `WPE_DMABUF_BUFFER_FORMAT` | `WPEPlatform/wpe/WPEToplevel.cpp:498` | format str | forces DMABuf format | N/A (SHM path) |
| `WPE_DISPLAY` / `WPE_PLATFORMS_PATH` | `WPEDisplay.cpp:187` / `WPEExtensions.cpp:62` | name / dir | select / locate WPE‑Platform backend modules | packaging |
| `WEBKIT_FORCE_VBLANK_TIMER` | `UIProcess/glib/DisplayVBlankMonitor.cpp:46` | set | timer‑based vblank instead of DRM | **optional**: no real vblank source |
| `WEBKIT_DISABLE_ASYNC_SCROLLING` | `WebProcess/.../DrawingAreaCoordinatedGraphics.cpp:218` | set | turn off async (threaded) scrolling | **optional**: may further calm scroll |

**Mesa‑level (not read by WebKit, act at the EGL/Gallium layer):** `GALLIUM_DRIVER=softpipe|llvmpipe`,
`MESA_LOADER_DRIVER_OVERRIDE`, `EGL_PLATFORM=surfaceless`. You already set these in
`scripts/run-wpeqt-on-device.sh:29‑31`.

### Why you get the SharedMemory transport (no DMABuf to crash)
The "Hardware"/DMABuf transport is only added when `usingWPEPlatformAPI && !renderDeviceFile.isEmpty()`
(`Source/WebKit/UIProcess/glib/WebProcessPoolGLib.cpp:120‑125`); with no GBM render node (guaranteed by
`LIBGL_ALWAYS_SOFTWARE=1`) the transport stays **SharedMemory**
(`AcceleratedBackingStoreDMABuf.cpp:198‑209` reports `RendererBufferFormat::Type::SharedMemory`). That is why
your `buffer-rendered` buffer is an SHM buffer whose `wpe_buffer_import_to_pixels()` returns CPU‑readable
BGRA — see §6.

### Recommended env for rmweb (no GPU, e‑ink)
```sh
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1     # explicit CPU Skia (WebProcessGLib.cpp:182)
export WEBKIT_SKIA_CPU_PAINTING_THREADS=0     # main-thread paint -> avoids Problem 3 (SkiaPaintingEngine.cpp:67)
export LIBGL_ALWAYS_SOFTWARE=1                # Mesa->llvmpipe/softpipe + WPE no-DRM-node (WPEDisplay.cpp:509)
export GALLIUM_DRIVER=softpipe                # or llvmpipe (Mesa-level; not read by WebKit)
export EGL_PLATFORM=surfaceless
# optional robustness:
export WEBKIT_FORCE_VBLANK_TIMER=1            # no real display vblank (DisplayVBlankMonitor.cpp:46)
# DO NOT set (absent in 2.48): WEBKIT_DISABLE_COMPOSITING_MODE / WEBKIT_FORCE_COMPOSITING_MODE
```
Your run script currently sets `GALLIUM_DRIVER`, `LIBGL_ALWAYS_SOFTWARE`, `EGL_PLATFORM` but **not**
`WEBKIT_SKIA_ENABLE_CPU_RENDERING` or `WEBKIT_SKIA_CPU_PAINTING_THREADS` — add those two.

---

## 6. The buffer you copy from (`buffer-rendered` handler) — lifetime rules

`wpe_buffer_import_to_pixels()` returns **`(transfer none)`** `GBytes`
(`Source/WebKit/WPEPlatform/wpe/WPEBuffer.cpp:266‑285`; SHM impl
`WPEBufferSHM.cpp:94‑106`). "transfer none" = **the buffer owns it; do not keep it past the handler**.
The pixels stay valid only until the presenter releases the buffer — which the headless timer can do on its
*next* tick. Rules for your `onBuffer` (`main.cpp:184‑214`), which are already correct and should stay:

- **Deep‑copy before returning** (`QImage(...).copy()`) — you do this (`:202‑203`). Keep it.
- `g_bytes_unref(bytes)` the returned `GBytes` (it *is* refcounted even though the data is borrowed) — you do
  (`:204`). Fine.
- **Do NOT call `wpe_view_buffer_released()` / `wpe_view_buffer_rendered()`** — you correctly avoid this
  (`:200‑201`). Confirmed: the headless view (`WPEViewHeadless.cpp:91,93`) **and** WebKit
  (`AcceleratedBackingStoreDMABuf.cpp:177‑193`) both already do it; a third call is the SEGFAULT (Problem 2).
- Memory order is **BGRA == `QImage::Format_ARGB32`** on little‑endian aarch64 — your comment/code are right.

So §6 needs no change; it documents *why* your existing handler is correct, so nobody "fixes" it back into a
crash.

---

## 7. If you later write your own panel `WPEView` (post‑headless)

When you replace headless with a real e‑ink `WPEView` subclass (to present straight to the panel instead of
copying through Qt), copy the **`WPEViewDRM`** template, not headless: implement `render_buffer` to stash
`pendingBuffer` and kick the e‑ink blit/waveform; in your "panel refresh done" callback (the e‑ink analog of
DRM's page‑flip), do the exact triplet:
```c
if (priv->committedBuffer) wpe_view_buffer_released(view, priv->committedBuffer);  // release previous
priv->committedBuffer = move(priv->pendingBuffer);                                  // promote
wpe_view_buffer_rendered(view, priv->committedBuffer);                              // frame‑done -> next paint
```
(`Source/WebKit/WPEPlatform/wpe/drm/WPEViewDRM.cpp:541‑550`.) Gating `buffer-rendered` on the **real e‑ink
refresh completion** gives you automatic frame‑pacing to the panel's true refresh rate — ideal for e‑ink, and
it removes the artificial 60 Hz timer.

---

## 8. Source index (all paths under `build/src/wpewebkit-2.48.5/`)

- `Source/WebKit/WPEPlatform/wpe/WPEView.cpp` — `render_buffer` dispatcher (878), `buffer-rendered`/`released`
  emit (901, 916), signal defs (372, 389), `set_visible`/`map`/`get_mapped` (686, 731, 713).
- `Source/WebKit/WPEPlatform/wpe/WPEView.h:58‑62` — `WPEViewClass::render_buffer` vfunc.
- `Source/WebKit/WPEPlatform/wpe/headless/WPEViewHeadless.cpp` — the self‑clocking 60 Hz loop (87‑99),
  `render_buffer` (115), toplevel→map (67‑81).
- `Source/WebKit/WPEPlatform/wpe/headless/WPEDisplayHeadless.cpp` — `create_view` (91), surfaceless EGL
  (146‑160), `wpe_display_headless_new` (192).
- `Source/WebKit/WPEPlatform/wpe/drm/WPEViewDRM.cpp:454‑493, 541‑550` — real‑panel template (page‑flip triplet).
- `Source/WebKit/WPEPlatform/wpe/WPEBuffer.cpp:266‑285` — `import_to_pixels` is `(transfer none)`.
- `Source/WebKit/UIProcess/wpe/AcceleratedBackingStoreDMABuf.cpp:58‑64, 152‑193` — the *consumer*: connects the
  signals, calls `wpe_view_render_buffer`, `frameDone`/`ReleaseBuffer`; SharedMemory format (198‑209).
- `Source/WebKit/UIProcess/API/wpe/WPEWebViewPlatform.cpp:58‑132` — builds the WPEView + backing store; maps
  `mapped`→`IsVisible` (67‑68, 84‑87); `callAfterNextPresentationUpdate` also hooks `buffer-rendered` (599).
- `Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp:852‑854` — `WEBKIT_TYPE_WEB_VIEW` + `"display"` ⇒
  `ViewPlatform`.
- `Source/WebKit/WebProcess/WebPage/AcceleratedSurface.cpp:54‑61` — DMABuf vs LibWPE surface selection.
- `Source/WebKit/WebProcess/glib/WebProcessGLib.cpp:175‑185` — `WEBKIT_SKIA_ENABLE_CPU_RENDERING`.
- `Source/WebCore/platform/graphics/skia/SkiaPaintingEngine.cpp:53‑68, 225‑268` — CPU/GPU thread pools.
- `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/DrawingAreaCoordinatedGraphics.cpp:64, 341` — painting
  suspended when not `IsVisible`.
- `Source/WebKit/WebProcess/WebPage/WebPage.cpp:4744‑4746` — compositing cannot be disabled.
- `Source/WebKit/UIProcess/API/glib/WebKitSettings.cpp:1561, 3873` — `hardware-acceleration-policy` is
  GTK‑only (no‑op on WPE).
- `NEWS` — 2.48.2/2.48.3 threaded‑rendering crash fixes; 2.48.4 headless/libdrm fix; 2.47.3 pause‑rendering.

### Web sources (secondary, for context)
- WPE 2.48 highlights (CPU default; `WEBKIT_SKIA_ENABLE_CPU_RENDERING=0` enables GPU; WPEPlatform is a
  **preview**, API may change): https://wpewebkit.org/blog/2025-04-11-wpewebkit-2.48.html
- WPE 2.46 highlights (CPU is the WPE default): https://wpewebkit.org/blog/2024-wpewebkit-2.46.html
- WPEPlatform visibility/map API added (Bugzilla 275482): https://bugs.webkit.org/show_bug.cgi?id=275482
- Skia CPU‑rendering EGL‑fence crash, fixed (Bugzilla 286566 / PR 39585):
  https://bugs.webkit.org/show_bug.cgi?id=286566 · https://github.com/WebKit/WebKit/pull/39585
- WebKit accelerated‑compositing uses surfaceless EGL + GBM/DMABUF (why software GL is fragile):
  https://blogs.igalia.com/carlosgc/2023/04/03/webkitgtk-accelerated-compositing-rendering/
- WPE env‑var reference (aperez): https://people.igalia.com/aperez/Documentation/wpe-webkit/environment-variables.html
- Igalia WPEPlatform tutorial (driving the toplevel after `get_wpe_view`):
  https://blogs.igalia.com/klee/building-a-custom-html-context-menu-with-the-new-wpeplatform-api/
- Cog headless reference (the *old* FDO model, for contrast):
  https://github.com/Igalia/cog/blob/master/platform/headless/cog-platform-headless.c (branch is **master**, not main)

---

## 9. Recommendations for problems 1 / 2 / 3 (actionable)

**Problem 1 — stall after ~2 frames.** Certain mechanism: the WebProcess **suspends painting unless
`ActivityState::IsVisible`** (`DrawingAreaCoordinatedGraphics.cpp:64`), and `IsVisible` is set from
`wpe_view_get_mapped()` (`WPEWebViewPlatform.cpp:67‑68`). A fresh headless `WebKitWebView` view comes up
**unmapped** (visible defaults FALSE) with a **0×0 toplevel** (headless default). Fix: after
`webkit_web_view_get_wpe_view`, **resize the toplevel to a real size first** (`wpe_toplevel_resize` +
`wpe_view_resized`), then force a real `set_visible(FALSE)`→`set_visible(TRUE)` transition, then **assert
`wpe_view_get_mapped()==TRUE` and width/height!=0**. After that, a DOM mutation *and* a normal scroll each
produce frames continuously (no content‑shift hack needed for visibility). (§3)

**Problem 2 — release SEGFAULT.** Cause: double/triple free — `WPEViewHeadless`'s timer
(`WPEViewHeadless.cpp:91`) **and** WebKit's `AcceleratedBackingStoreDMABuf`
(`AcceleratedBackingStoreDMABuf.cpp:183‑193`) already release every buffer. Fix: **never** call
`wpe_view_buffer_released()` (or `wpe_view_buffer_rendered()`) when embedding `WebKitWebView`. Only copy pixels
out and `g_bytes_unref` the borrowed `GBytes`. Your current handler is already correct — keep it that way. (§2, §6)

**Problem 3 — scrolled render crash on software GL.** Cause: scroll → large multi‑region repaint → WPE 2.48
**threaded Skia WorkerPool** path (made thread‑safe only across 2.48.2/2.48.3; the compositor also uses EGL on
softpipe). Fix: set **`WEBKIT_SKIA_CPU_PAINTING_THREADS=0`** (main‑thread paint, no WorkerPool —
`SkiaPaintingEngine.cpp:67`) plus **`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`**, and stay on the newest 2.48.x.
Then test real `scrollY>0`; if stable, drop the content‑shift hack. Do **not** try
`WEBKIT_DISABLE_COMPOSITING_MODE` — it does not exist in 2.48 and compositing can't be disabled
(`WebPage.cpp:4744‑4746`). (§4, §5)
