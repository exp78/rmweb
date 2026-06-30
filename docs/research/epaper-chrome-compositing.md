# Chrome over the web frame on the reMarkable epaper scenegraph — research (2026-06-30)

> Investigated after "chrome OR page, never both": our QtQuick toolbar never appeared on the e-ink
> panel alongside the full-screen web frame (user saw "just the page"; `grabWindow` only captured the
> web item; even a hardcoded black bar / a z:1000 red square didn't show). Three parallel research
> agents: (1) the official WPE→Qt integration, (2) the `libqsgepaper` scenegraph, (3) chrome-as-HTML.
>
> **Headline: QtQuick chrome CAN composite on this backend — there's an official template that proves
> a toolbar-over-content works. Our chrome was visually erased by the full-screen `QQuickPaintedItem`
> damage rect + a forced-full refresh (+ the wrong layout pattern), NOT a fundamental limit.** We were
> not fundamentally wrong; the chrome just needs the proven epaper pattern (Option A) — or the
> chrome-as-HTML pivot (Option B). The "proper" WPE-Qt item (WPEQtView) needs a GPU and is out.

## The question we asked
Was our architecture — WPE headless → BGRA `QImage` → `QQuickPaintedItem` (`WpeView`) → Qt6 epaper QPA
(`QT_QPA_PLATFORM=epaper`, `QT_QUICK_BACKEND=epaper`, `libqsgepaper`) — the wrong way to put browser
chrome (address bar, buttons) over the page?

## Finding 1 — The official WPE→Qt item (`WPEQtView`) needs a GPU; NOT viable here
- Igalia ships a maintained **Qt6 `WPEQtView`** — a real `QSGSimpleTextureNode` QML item that composites
  with QML chrome natively (the "real QML WebView" we suspected exists). Repos:
  WebKit `Source/WebKit/UIProcess/API/wpe/qt6/`; standalone `github.com/nowrep/wpewebkit-qt`.
- But it **hard-requires OpenGL(ES) + EGL**: it calls `QSGRendererInterface::OpenGLContextResource` and
  aborts with *"Cannot retrieve OpenGL context via Qt"* on a non-GL scenegraph; uploads frames via
  `wpe_buffer_import_to_egl_image` + `glEGLImageTargetTexture2DOES`. **No software/QImage path.**
- The rMPP has **no EGL/GLES/Wayland** (only `libdrm`); the epaper scenegraph is software-only. Using
  WPEQtView would mean running QtQuick on GL/RHI over llvmpipe-EGL **and dropping the epaper QPA's e-ink
  packing/waveforms** (we'd drive the panel ourselves for the whole composited frame) — a large,
  high-risk change that reintroduces full-scene CPU raster cost. **Verdict: not our path.**
  (The `qtwpe` QPA plugin = the same thing, officially "no more supported".)

## Finding 2 — `libqsgepaper` DOES composite all nodes; our chrome was erased by full-screen damage + forced-full refresh ★ KEY
- `libqsgepaper` is a **pure-CPU `QPainter` scenegraph compositor**: per-type node subclasses
  (`EPRectangleNode`, `EPGlyphNode`, `EPImageNode`, `EPPainterNode`) each `draw(QPainter*)` into ONE
  shared `QImage` (`EPFrameBuffer::framebuffer()`), walked in z-order by `EPRenderer`. It then calls
  `EPFrameBuffer::sendUpdate(QRect damage, waveform, mode, …)` to drive the e-ink waveform for that rect.
- **The toolbar is NOT culled** — it's drawn into the shared `QImage` like any node, and
  `EPPainterNode::setOpaquePainting` only toggles that item's own background blend (it does not prune
  siblings). The official **`Eeems-Org/remarkable-template-qt-app` runs a toolbar over content on this
  exact backend and it works** — `ApplicationWindow{ header: Rectangle{RowLayout{Label}};
  contentData:[Rectangle{Text}] }`.
- **Why OURS didn't show:** our full-screen `QQuickPaintedItem` **dirties its whole rect every frame →
  the damage rect = the whole screen → a full-panel refresh** (amplified by any `setForceFull`) → the
  chrome, though present in the shared `QImage`, is visually dominated / "flashed away". (dragly
  documented the identical "full-bleed PaintedItem → whole-screen refresh" failure; fix = small
  per-region damage rects / tiling.) Secondary suspects (since the black-bar probe was absent from the
  grab too): the painted item's opaque background / paint-order, or our `ColumnLayout`+`ToolBar` sizing
  vs the proven `header`/`contentData` + plain-`Rectangle` pattern.
- `QQuickPaintedItem` is the **trigger, not a hard blocker** (its pixels are CPU-readable; siblings are
  not culled). **Do NOT use `layer.enabled`** — the software backend has no layer/texture path beyond
  `EPImageNode`; a layered subtree is pure overhead.

### Option A — keep QtQuick chrome (cheapest; PROVEN to work on this backend)
1. **Adopt the proven pattern:** `ApplicationWindow { header/footer: <chrome>; contentData: [<page item>] }`
   with plain `Rectangle`/`Text`/`Image` (verify `ToolBar` renders; the working template uses plain
   `Rectangle`). **Stop globally forcing a full refresh**; refresh the page region and the chrome region
   as **separate `sendUpdate` rects**.
2. **Decouple refresh regions (and go faster):** move the web frame off `QQuickPaintedItem` onto a
   `QSGSimpleTextureNode` (`window()->createTextureFromImage(m_img)`) or a plain `Image` fed by a
   `QQuickImageProvider` (`image://wpe/frame?<seq>`, `cache:false`) — the `EPImageNode` path xochitl uses
   for bitmaps. Value = damage-region/refresh control + speed, not "fixing unreadable pixels".
3. **Probe to choose 1 vs 2:** grab + a `Rectangle{z:1000}` overlay. In-grab-but-not-on-panel → it's the
   full-refresh clobber → fix #1 suffices. Absent-from-grab → paint-order/opaque → set the painted item
   non-opaque + check stacking, or do #2.
4. Last resort: chrome in a **separate top-level `Window`** over the page window (serialize presents — we
   already hit the present-overlap deadlock).

## Finding 3 — Chrome-as-HTML (two WebViews) — robust alternative that sidesteps compositing
- Since only the web frame reaches the panel, render the chrome IN the web layer. Cleanest = **two
  `WebKitWebView`s** (WPE supports per-view frame callbacks + transparent overlay via
  `webkit_web_view_set_background_color(…, alpha=0)`):
  - **CONTENT** view: opaque; **top-level** `webkit_web_view_load_uri` (NOT an iframe → sidesteps
    X-Frame-Options; the address bar tracks the real URL via `load-changed`/`notify::uri`).
  - **CHROME** view: transparent overlay; loaded from a **trusted scheme** (`rmweb-ui://` or a locked
    `file://`, never http(s)); holds the ONLY `UserContentManager` message-handler broker; address bar +
    back/fwd/reload + an **on-screen keyboard rendered as HTML**.
  - Composite the two BGRA buffers in C++ (A opaque, B alpha over A) → epaper; hit-test taps
    (bar region → B, page → A) and inject via our existing finger-evdev path.
- This is the **Firefox OS / Gaia** pattern. Isolation (ALL must hold): privilege by scheme/origin (not
  "it's HTML"); structurally sever untrusted content (separate context, no embedder reference); a
  **separate WebProcess per view**; a narrow, async, validated broker (never a generic "eval"); treat the
  chrome HTML as an XSS target (strict CSP, no `eval`/`innerHTML` of remote strings). **Avoid
  one-view-iframe** (X-Frame-Options blanks too many sites; address bar can't track iframe nav; injecting
  chrome into the untrusted DOM is the anti-pattern — nyxt's RCE is the warning).
- Cost: a 2nd WebProcess (RAM) + 2-buffer compositing on a CPU-only device (chrome is tiny/static →
  dedup re-renders). Bonus: solves on-screen text entry (HTML keyboard) — the Wayland OSK path doesn't
  exist under the epaper QPA anyway.

## Recommendation
1. **Try Option A first** — it's cheap and there is **proof it works on this exact backend** (the
   Eeems-Org template). Switch to `ApplicationWindow header/contentData` + plain-Rectangle chrome, stop
   forcing full refresh, scope damage rects; if still erased, move the frame to a texture/`Image` node.
2. **Fallback to Option B** (chrome-as-HTML, two WebViews) if A keeps fighting — robust, and it also
   gives us the on-screen keyboard for free.
3. **Skip `WPEQtView`** (GPU/EGL-only).
**We were not fundamentally wrong** — the engine path (A0/A4/A5/A6) is fine; only the chrome needs the
proven epaper pattern or the HTML-chrome pivot.

## Addendum (2026-06-30, on-device) — Option A FAILED for our live WpeView → go Option B; the QImage IS the panel
Tested Option A across **every** variant on the device: `ColumnLayout`+`ToolBar`, `ApplicationWindow.header`
(with `height` AND with `implicitHeight`), plain black `Rectangle`, a `z:1000` overlay `Rectangle`.
**Chrome never appears — neither on the panel nor in `grabWindow()`.** Critically, `grabWindow()` under the
epaper QPA only ever captures the **WpeView** (the `QQuickPaintedItem`); no sibling / overlay / header
QtQuick item is in the grab, and the web content is never shifted down by a header. So the static-content
`Eeems` template's success does NOT transfer: **our live `QQuickPaintedItem` + the A6 present is the ONLY
thing reaching the panel; QtQuick chrome does not composite with it.** (The remaining untried A lever —
feeding the frame through an `Image`/`QQuickImageProvider`/`QSGSimpleTextureNode` instead of
`QQuickPaintedItem` — might change this, but is uncertain and was deprioritised.)

**Conclusion: the WpeView's QImage IS what reaches the panel → draw the chrome INTO that QImage.**
- **B2 (recommended — guaranteed + smallest): C++ `QPainter` chrome.** When chrome is summoned, paint a
  toolbar (back / forward / reload + page/positions) onto the BGRA `QImage` before it is displayed;
  hit-test taps in the toolbar rect in C++ → engine actions. **Guaranteed to render — it's literally the
  frame.** URL entry: defer at first, or add a `QPainter`-drawn on-screen keyboard (hit-tested) later.
- **B1 (fuller, later): two `WebKitWebView`s** — transparent HTML chrome over the content view (the
  researched design); richer chrome + HTML OSK, at the cost of a 2nd WebProcess + compositing two buffers.

## Sources
- WPE-Qt: WebKit `Source/WebKit/UIProcess/API/wpe/qt6/WPEQtView.cpp`; `github.com/nowrep/wpewebkit-qt`;
  base-art.net "Introducing WPEQt"; wpewebkit.org 2.51.90/2.52.2 notes; Igalia/meta-webkit discussion #236;
  ST community "Qt-wpe is no more supported"; Qt WebEngine overview; QtWebView "overlapping not supported".
- libqsgepaper: canselcik/libremarkable `reference-material/libqsgepaper.md` (symbol dump);
  `pl-semiotics/libqsgepaper-snoop` (framebuffer=QImage, clearScreen=QImage::fill, sendUpdate single-QRect);
  **`Eeems-Org/remarkable-template-qt-app`** (toolbar-over-content works); dragly.org 2017-12-01
  (full-bleed PaintedItem → whole-screen refresh + grid workaround); `reMarkable/epaper-qpa`;
  developer.remarkable.com Qt epaper guide; Qt docs (QQuickPaintedItem renders to QImage in Qt6; software
  backend has no ShaderEffect/layers).
- chrome-as-HTML: MDN Firefox OS Apps architecture + Browser API; Firefox scriptSecurity/jsactors;
  Electron security/context-isolation; Chromium WebUI explainer; webOS architecture; WPE architecture +
  `set_background_color` (bug 192305) + UserContentManager message handlers; Igalia/klee custom-HTML
  context-menu; EinkBro/KOReader/netsurf-reMarkable (native chrome); MDN X-Frame-Options.
