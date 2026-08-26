# E-ink reading UX & touch input — actionable reference (rmweb / reMarkable Paper Pro)

Scope: how mature e-ink readers (KOReader, Plato, Kindle/Kobo/Pocketbook, netsurf-reMarkable) handle
**paging vs scrolling, touch gestures, and refresh**, distilled into concrete, opinionated recommendations
for *our* WPE-WebKit reading browser. Hardware: 1620×2160 colour e-ink (~228 DPI), Qt6 QtQuick + reMarkable
`epaper` QPA, Elan capacitive multitouch + Wacom/Elan pen, **GPU on die but no driver in the stock OS** (CPU-only in practice).

Companion docs: `docs/research-reuse.md` §2/§2a (refresh API & control loop — the *how* on rMPP), §"Input"
(verified event-node map + coordinate transforms), `docs/device-profile.md` (hardware facts).
**This doc is the UX/interaction layer; research-reuse.md is the hardware/refresh layer. Don't duplicate — cross-reference.**

---

## TL;DR — what to build

1. **PAGE, don't smooth-scroll.** The whole e-ink reader ecosystem flips discrete full-screen pages.
   Continuous pixel-by-pixel scroll ghosts badly and spends one slow refresh per frame. For our browser
   (which re-renders in the engine per flip anyway) paging is *also* the cheapest: one render + one refresh per turn.
2. **Tap zones + horizontal swipe** to turn pages. Right ~⅔ / bottom = next, left ~¼ / top = previous
   (KOReader default split). Swipe **west = next, east = previous** (Kindle/Kobo/KOReader/Plato consensus).
3. **Thresholds (DPI-scaled to ~228 DPI):** tap-vs-swipe distance ≈ **35 px pan threshold / ~50–60 px to commit a swipe**;
   swipe must complete within **≤ 900 ms**; double-tap window **300 ms / ≤ 50 px**; long-press **500 ms**.
   Debounce identical page-turns by ~**250–400 ms** to kill double-fires on a slow panel.
4. **Refresh:** fast **mono partial** for chrome/highlight; **colour full flash** on every page turn (develops
   colour + clears ghosts), OR partial-on-turn with a **full flash every N=6–12 turns** if you want speed over fidelity.
5. **Qt6 touch:** make the page a custom item with `setAcceptTouchEvents(true)` + `setAcceptedMouseButtons(Qt::AllButtons)`,
   handle **both** `touchEvent()` and `mousePressEvent()/mouseReleaseEvent()` (the epaper QPA may deliver *either*),
   do gesture math yourself. Prefer **TapHandler/DragHandler (Pointer Handlers)** over `MouseArea` if you stay in QML.

---

## 1. Scrolling vs paging on e-ink

### The prevailing model: discrete paging
Every mature e-ink *book* reader pages by default. KOReader's `ReaderPaging` turns pages via
`onGotoViewRel(1)` / `onGotoViewRel(-1)` (next/prev) — a full re-render and refresh, not a scroll.
Plato divides the screen into nine regions and a page turn is a discrete jump. Kindle "EasyReach", Kobo,
Pocketbook all use invisible left/right tap zones to flip whole pages. Reason: each changed pixel costs a
slow waveform refresh, and a *moving* image leaves ghost trails because partial waveforms don't fully reset ink.

### What flips a page (cross-reader consensus)
- **Tap zones** (primary, fastest):
  - **Kindle (EasyReach):** tap **right** zone → next, **left** zone → previous. ([thomaspark.co](https://thomaspark.co/2012/01/kindle-touch-gestures/))
  - **Kobo:** tap **right** → next, **left** → previous; swipe right→left = next, left→right = previous.
    ([Kobo help](https://help.kobo.com/hc/en-us/articles/360017481194-Turn-pages-on-your-eReader))
  - **KOReader default zones** (from `defaults.lua`, ratios of screen w/h):
    - `DTAP_ZONE_FORWARD  = {x=1/4, y=0, w=3/4, h=1}` — **right ¾ of screen → next page**
    - `DTAP_ZONE_BACKWARD = {x=0,   y=0, w=1/4, h=1}` — **left ¼ → previous page**
    - Corner zones `{w=1/8, h=1/8}` (TOP_LEFT/TOP_RIGHT/BOTTOM_LEFT/BOTTOM_RIGHT) → menus/bookmarks/screenshot.
    ([defaults.lua](https://github.com/koreader/koreader/blob/master/defaults.lua))
  - **Plato 9-region grid:** West Strip = prev, East Strip = next, North/South strips + Center = toggle bars,
    corners = bookmark / ToC / go-to-page / prev-location.
    ([Plato MANUAL.md](https://github.com/baskerville/plato/blob/master/doc/MANUAL.md))
- **Swipe** (secondary): **swipe west (right→left) = next, swipe east (left→right) = previous** — KOReader
  `onSwipe` ("west" advances, "east" backward), Plato ("swipe west/east to go to next/previous page"), Kobo, Kindle.
  Note KOReader: the **swipe gesture spans the whole screen** and ignores tap zones (you must lift a finger to
  fire a swipe), so swipe and tap-zone are orthogonal — that's the right split. ([KOReader gestures discussion](https://www.ereadersforum.com/threads/how-to-customize-page-turns-and-gestures-in-koreader-on-any-e-reader.7807/))

### "Overlap" between pages
Only relevant in *continuous-scroll* mode (which we're avoiding). When a reader does scroll-paging it repeats a
strip at the top so the reader doesn't lose their place: **KOReader `DOVERLAPPIXELS = 30`** px repeated at the top
of each new screen ([defaults.lua](https://github.com/koreader/koreader/blob/master/defaults.lua)). For true
*paging* there is no overlap. If we ever offer a "scroll by ~90% viewport" mode, repeat ~**30–60 px** (scale to DPI)
so a line is never split across the seam.

### Does anyone do continuous scroll, and how do they survive it?
Yes, optionally, and they mitigate hard:
- KOReader "scroll mode" exists but uses **inertial scroll with an activation delay** (`scroll_activation_delay`,
  `_pan_activation_time`) so a slow finger drag doesn't fire a scroll, and it coalesces. Even so it ghosts and most
  users page.
- The universal mitigation = **fewer, larger updates + periodic full flash** (see §3). netsurf-reMarkable accumulates
  a dirty box and emits **one** framebuffer update per tick rather than per pixel (research-reuse.md §2).

### → Recommendation for rmweb
- **MVP = paging only.** Map page-down to **`window.scrollBy(0, viewportHeight − overlap)`** inside the WPE page
  (overlap ≈ 48 px at 228 DPI), then trigger **one** colour full-refresh. One engine render + one refresh per flip.
  Crucially: drive the scroll **inside the page via JS injection / WebKit API**, not by moving a Qt viewport, so the
  engine produces a single clean final frame to push to the panel (no intermediate frames → no ghost trails).
- Track scroll position so "next" at page bottom advances and "previous" reverses symmetrically; clamp at doc ends.
- **Defer real smooth-scroll to a later phase.** If added, gate it behind inertial activation + dirty-box coalescing
  + the anti-ghost full-flush loop (research-reuse.md §2a), and use a **mono fast** waveform while moving, colour full
  when motion stops. Do **not** scroll on every touchmove frame — sample/throttle to ~1 update per refresh-period.

---

## 2. Touch gesture handling best practices on e-ink

The hard problem on e-ink isn't recognition — it's **avoiding accidental page turns** (a misfire costs a ~1 s
full-screen flash) and **palm/pen rejection** (you rest a hand or hover the pen while reading).

### KOReader's gesture thresholds — the reference numbers
From `frontend/device/gesturedetector.lua` (values are DPI-scaled via `scaleByDPI`, so treat as @ a baseline DPI
and scale to our 228):

| Constant | Default | Meaning |
|---|---|---|
| `ges_tap_interval` (TAP_INTERVAL) | **0 ms** (tunable, users set ~300–400 ms) | min gap between taps to debounce hardware bounce ([PR #6798](https://github.com/koreader/koreader/pull/6798)) |
| `ges_double_tap_interval` | **300 ms** | max gap for a double-tap |
| `DOUBLE_TAP_DISTANCE` | **50 px** | max travel between the two taps of a double-tap |
| `SINGLE_TAP_BOUNCE_DISTANCE` | **50 px** | travel under which a re-contact is treated as bounce, not a new tap |
| `ges_hold_interval` (HOLD_INTERVAL) | **500 ms** | press duration → long-press/hold (text select) |
| `PAN_THRESHOLD` | **35 px** | movement that converts a tap/hold into a pan (drag) |
| `ges_swipe_interval` (SWIPE_INTERVAL) | **900 ms** | a swipe must complete (lift) within this window |
| `MULTISWIPE_THRESHOLD` | **50 px** | leg length before a multiswipe registers a direction change |
| `TWO_FINGER_TAP` region / duration | **20 px / 300 ms** | two-finger tap tolerance |

([gesturedetector.lua](https://github.com/koreader/koreader/blob/master/frontend/device/gesturedetector.lua))

### Swipe vs tap discrimination (the algorithm to copy)
1. On **contact down** → enter *tap/hold* candidate state, record `(x0, y0, t0)`.
2. If movement exceeds **PAN_THRESHOLD (~35 px)** before lift → it's a **pan/drag**, not a tap. Cancel any tap.
3. On **lift**:
   - If total travel < tap distance **and** within tap interval → **TAP** (route to whichever zone it's in).
   - If travel ≥ swipe-commit distance **and** `Δt ≤ 900 ms` **and** displacement is non-zero → **SWIPE**;
     direction from the dominant axis: `west` if `Δx<0`, `east` if `Δx>0`, `north`/`south` if `Δy` dominates
     (diagonal when both legs are comparable). KOReader: *"if time_diff < ges_swipe_interval … if x_diff ≠ 0 or
     y_diff ≠ 0 then return true"*. ([gesturedetector.lua](https://github.com/koreader/koreader/blob/master/frontend/device/gesturedetector.lua))
   - If neither and contact stayed put ≥ **500 ms** before lift → **HOLD**.
4. **Axis bias for reading:** only horizontal swipes flip pages; require horizontal displacement to dominate vertical
   (e.g. `|Δx| > 1.5·|Δy|`) so a sloppy vertical drag never turns a page.

### Concrete numbers for rmweb (scaled to 228 DPI)
- **Swipe commit distance:** **~60–80 px** (≈ 8–9 mm). Bigger than KOReader's 35 px pan threshold on purpose — on a
  big slow panel you want a *deliberate* swipe, not a twitch. Use **PAN start = 35 px**, **SWIPE commit = 60 px**.
- **Swipe time window:** **≤ 700 ms** (slightly tighter than 900 so a slow exploratory drag isn't a page turn).
- **Tap max travel:** **≤ 25 px**; **tap max duration:** **≤ 250 ms** (longer → hold).
- **Double-tap:** **≤ 300 ms / ≤ 50 px** (e.g. reserve for "full refresh" or zoom).
- **Long-press:** **≥ 500 ms** stationary (reserve for link context / select later).
- **Page-turn debounce:** ignore a repeat turn in the **same direction within 250–400 ms** of the last commit
   (prevents a partial finger roll from flipping twice while the panel is still flashing). KOReader added a tap
   interval setting for exactly this hardware-bounce reason. ([PR #5138](https://github.com/koreader/koreader/pull/5138),
   [PR #6798](https://github.com/koreader/koreader/pull/6798))

### Palm / pen rejection
- **Use touch-major / contact size if exposed** (Elan reports ABS_MT_TOUCH_MAJOR); reject contacts above a palm
  threshold. If not exposed, **ignore multi-finger contacts during reading** except recognised 2-finger gestures
  (a palm registers as several large/short-lived slots) — Plato treats simultaneous-landing 2-finger as deliberate
  (rotation) and otherwise tracks discrete slots.
- **Pen vs touch are separate evdev devices** on reMarkable (pen = "Elan marker input", touch = "Elan touch input";
  research-reuse.md §"Input"). **Suppress touch page-turns while the pen is in proximity/contact** — when the user is
  annotating or hovering, finger contacts are almost always palm. This is the single biggest accidental-turn source.
  In Qt6 you can distinguish by `event->pointingDevice()->type()` (Pen vs TouchScreen).
- **Open input shared, NOT `EVIOCGRAB`** (research-reuse.md §"Input") so the system/QPA still works; do rejection in
  our handler.
- **Edge guard:** ignore contacts that *start* within ~8–10 px of a screen edge (bezel palm contact / accidental grip).

---

## 3. Refresh strategy tied to UX

Full hardware details (rMPP `EPFramebuffer::swapBuffers(rect, screenMode, flags)` — older builds add a
`contentType` arg after the rect — waveform enums,
the debounce + anti-ghost control loop) are in **research-reuse.md §2 / §2a — do not duplicate.** This section is the
*UX mapping*: which refresh class each interaction uses.

### The core trade-off
- **Partial / fast (mono, DU/A2-class):** ~250–350 ms, no flash, but **ghosts** (faint residue of prior content).
- **Full flash (colour, GC16/QualityFull):** ~1 s, black→white→content, **clears all ghosts + develops colour**.
  Colour (Gallery-3/ACeP) content *requires* a full multi-pass waveform or it stays pale/white (research-reuse.md §1).

### "Full refresh every N pages" — the KOReader pattern
KOReader's `FULL_REFRESH_COUNT` promotes every Nth partial refresh to a full flash to clear accumulated ghosting,
and **always full-flashes pages containing images** ("to get rid of residue and ghosting from the previous page").
Users tune N; "never / every 1 / every 6 / on new chapter" are typical. Any partial counts toward the promotion.
([KOReader e-ink settings](https://koreader.rocks/user_guide/),
[Issue #10047](https://github.com/koreader/koreader/issues/10047),
[Issue #13863](https://github.com/koreader/koreader/issues/13863))

### → Recommendation for rmweb (interaction → refresh class)

| Interaction | Content | Refresh class | Why |
|---|---|---|---|
| **Page turn / navigation** | colour | **colour FULL flash** every turn (default) | web pages are colour + image-heavy → must develop colour & clear ghosts; ~1 s matches stock reMarkable feel |
| Page turn (speed-priority mode, opt-in) | mostly text | **mono fast partial**, with **full flash every N=6–12 turns** | faster flips, accept some ghosting; the periodic flash is the KOReader `FULL_REFRESH_COUNT` insurance |
| Tap-zone highlight / link feedback | mono, bounded | **mono fast partial**, region only | snappy, no flash; keep the dirty rect tight (a too-large partial gets force-promoted to a flash) |
| Toolbar / URL bar / page indicator update | mono | **mono fast partial**, region only | chrome must feel responsive |
| Image/colour region settled after a fast partial | colour | re-do **just that region** in colour once motion stops | "settle to colour" so it's not left pale |
| Periodic anti-ghost flush | colour | **colour FULL flash** | the every-N safety net + on idle |

**Rules of thumb:**
- **A page turn is the natural place to spend a full flash** — the user expects a beat when turning a page, so put the
  slow colour refresh there and nowhere else.
- **Keep partials small.** A partial covering most of the screen looks like (and often gets promoted to) a flash, so
  scope dirty rects tightly — KOReader explicitly warns large partials trigger "spurious large black flashes".
- **Counter resets on every full.** Maintain `partials_since_full`; on turn either (a) full-flash → reset, or
  (b) partial → increment, and force a full when it hits N (research-reuse.md §2a step "FULL_REFRESH_COUNT").
- **Full-flash on first paint of a new page/site and on returning from chrome → page** (analogous to KOReader's
  "flash on images / new chapter").

---

## 4. Qt6 QtQuick touch specifics (custom item under the epaper QPA)

### What the epaper QPA actually delivers
reMarkable's official Qt Quick guide says **"Touch event handling works out of the box"** and its own example uses a
plain **`MouseArea` with `onPressed`** — i.e. the stack happily delivers *mouse-synthesized* events for simple taps.
([reMarkable Qt epaper docs](https://developer.remarkable.com/documentation/qt_epaper)) Touch arrives via Qt's
**evdevtouch** handler, configured by `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` (can pin the device node, e.g.
`/dev/input/event3`, plus `rotate=…:invertx`); evdevtouch generates **true multi-touch `QTouchEvent`s** (unlike the
old tslib path which only made mouse events).
([Qt embedded inputs](https://doc.qt.io/qt-6/inputs-linux-device.html)) Our own recon (research-reuse.md §"Input")
confirms the epaper QPA feeds touch through evdev and the touch device is event3 ("Elan touch input"; event2 is the pen).

> ⚠️ **The crux:** depending on whether evdevtouch registers and whether anything consumes the `QTouchEvent` first,
> your custom item may receive **`QTouchEvent` OR only synthesized `QMouseEvent`**. Design for **both.**

### The Qt6 behaviour changes that bite (cite & obey)
- **Qt6 requires `setAcceptTouchEvents(true)`** explicitly. In Qt5, `setAcceptedMouseButtons()` *implicitly* opted an
  item into touch; **in Qt6 it does not** — without `setAcceptTouchEvents(true)` your `touchEvent()` **never fires**.
  ([QQuickItem docs](https://doc.qt.io/qt-6/qquickitem.html))
- **TouchBegin must be accepted** or no further touch points are delivered: "TouchUpdate/TouchEnd are sent to the item
  that accepted TouchBegin; if TouchBegin is not accepted … no further touch events are sent until the next TouchBegin."
  ([QTouchEvent docs](https://doc.qt.io/qt-6/qtouchevent.html)) → **accept the event in `touchEvent()`.**
- **`MouseArea` is mouse-only** and depends on synthetic mouse events: *"Multi-touch still doesn't work with the
  remaining mouse-only items like MouseArea."* Qt recommends **Pointer Handlers** (TapHandler, PointHandler,
  DragHandler) instead. ([Qt6 input-events blog](https://www.qt.io/blog/input-events-in-qt-6))
- **Detect synthesized events** via `event->device()->type()` / `pointerType()` (Qt5's `QMouseEvent::source()` is gone).
  ([Qt6 input-events blog](https://www.qt.io/blog/input-events-in-qt-6))
- **QPA must register input devices** so events carry device identity; this is "incomplete across platforms," which is
  exactly why a custom/embedded QPA can silently fail to deliver proper `QTouchEvent`s.
  ([Qt6 input-events blog](https://www.qt.io/blog/input-events-in-qt-6))
- Known real-world failure: synthesized mouse from touch **"can stop at seemingly random times,"** leaving an app
  unresponsive — another reason not to rely on synthesized mouse alone.
  ([Qt forum](https://forum.qt.io/topic/96780/qt-widgets-not-responding-to-touch-event-sometimes))

### Approaches compared (for our full-screen page item)

| Approach | Gets touch? | Verdict for rmweb |
|---|---|---|
| **`QQuickPaintedItem::touchEvent()`** + `setAcceptTouchEvents(true)` + `setAcceptedMouseButtons(Qt::AllButtons)` + override `mousePress/Release` | Yes (touch) **and** mouse fallback | ✅ **Recommended.** Our page is already a `QQuickPaintedItem` (`WpeView`, research-reuse.md §3). Do gesture math in C++; works whether QPA sends touch or mouse. |
| `MouseArea` (QML) | Mouse only (incl. synthesized) | ⚠️ Fine for *taps* (it's what reMarkable's sample uses) but **no multi-touch, fragile for swipe discrimination**, and the synth path can stall. Acceptable as a *fallback path*, not the primary. |
| `MultiPointTouchArea` (QML) | Touch only | ❌ Breaks if QPA delivers only mouse — no fallback. |
| **`TapHandler` + `DragHandler`/`PointHandler`** (QML, on the page item) | Both (handlers consume mouse+touch) | ✅ **Best if you stay in QML.** Qt's recommended path; TapHandler→tap zones, DragHandler→swipe. Use these over MouseArea. |
| `SwipeView` | n/a | ❌ It's a paged *container of pages you pre-build*; we render one page on demand in the engine — wrong abstraction. |
| `Flickable` | Touch/mouse | ❌ Implies smooth scroll (the thing we're avoiding); also fights our engine-driven scroll. |

### Robust recipe (do this)
1. On the page item (the `QQuickPaintedItem`): in the constructor call
   **`setAcceptTouchEvents(true);` and `setAcceptedMouseButtons(Qt::AllButtons);`** (and `setAcceptHoverEvents(true)`
   only if you need pen hover). The forum solution that finally made `touchEvent` fire used
   `setAcceptedMouseButtons(Qt::AllButtons)` + accept-events.
   ([Qt forum #79364](https://forum.qt.io/topic/79364/touch-gesture-recognition-with-a-custom-qquickpainteditem))
2. Override **`touchEvent()`** — **accept the `TouchBegin`** (call `event->accept()`), then run the §2 state machine on
   the first touch point (`points().first()`), distinguishing pen via `pointingDevice()->type()`.
3. **Also override `mousePressEvent`/`mouseMoveEvent`/`mouseReleaseEvent`** and feed the *same* gesture state machine,
   so if the QPA only sends synthesized mouse, tap/swipe still work. Guard against double-handling by checking
   `event->device()->type()` (skip mouse that is `TouchScreen`-sourced if you already handled the touch).
4. **Disable Qt's mouse-event compression for moves** if you sample positions yourself
   (`QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false)` / `AA_CompressTabletEvents`) — but
   honestly we *want* throttling on e-ink, so leave compression on and additionally throttle scroll updates to ~1/refresh.
5. **Pin the touch device node** via `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event3` (touch = event3
   "Elan touch input", pen = event2; verified on device — research-reuse.md §"Input") so evdevtouch binds the right
   device and emits real `QTouchEvent`s rather than nothing.
6. **Verify on device with `evtest`** which node is touch vs pen before wiring (research-reuse.md warns the exact
   event→device map must be confirmed on the rMPP).

---

## 5. Minimal reading-browser chrome for e-ink

Constraints: no comfortable on-screen keyboard, every redraw is slow, distraction-free reading is the point.
Reference patterns: netsurf-reMarkable hides chrome and reveals a menu bar by **swiping ~1 cm down from the top
edge**, and shows a tiny **'a'** in the bottom-right to summon the keyboard
([netsurf-reMarkable README](https://github.com/alex0809/netsurf-reMarkable/blob/main/README.md)). KOReader/Plato
keep all chrome **hidden by default**, revealed by tapping a top/bottom strip or corner (Plato: tap North/South strip
or Center → toggle bars; corners → ToC, bookmark, go-to-page).

### → Recommendation for rmweb
- **Chrome hidden while reading.** The page owns the full 1620×2160. Reveal a top bar by **tapping the top strip**
  (reuse KOReader's `{y=0, h=1/8}` corner/edge idea) or a **short swipe-down from the top edge** (netsurf pattern).
  Auto-hide after an action or a few seconds (one partial refresh to remove it).
- **Top bar contents (one row, large hit targets ≥ ~48 px):** `[◀ back] [▶ fwd] [⟳ reload] [URL / title, tappable] [✕]`.
  Use a **mono fast partial** for showing/hiding the bar (it's bounded, no flash).
- **Page indicator:** a thin progress strip / "p N" at a bottom corner, updated with a tiny partial on each turn —
  don't full-flash for the indicator.
- **URL entry without a good keyboard:**
  - Primary = **link navigation by tapping links in the page** (this is a *reading* browser).
  - Secondary = a **bookmarks/start page** (a few large tiles) so the user rarely types a URL.
  - When typing is unavoidable, show the **system/QtVirtualKeyboard full-screen**, accept the URL, **then full-flash**
    once on navigate. Keep an autocomplete/history list of big tappable rows to minimise keystrokes.
- **Full-refresh affordance:** like Plato's "tap bottom-left + top-right corners → full refresh," give an explicit
  **"clear ghosting" gesture** (e.g. **two-finger tap** or a chrome button) that forces a colour full flash on demand.
- **No hover/animation chrome.** Inject `* {animation:none!important; transition:none!important; scroll-behavior:auto!important}`
  + emulate `prefers-reduced-motion: reduce` in WPE (research-reuse.md §2a step 4) so CSS never spams partial refreshes.

---

## What NOT to do (others' mistakes to avoid)
- ❌ **Don't smooth-scroll by moving a Qt viewport** — you'll emit a frame per touchmove → ghost trails + a refresh storm.
  Drive scroll *inside the WPE page* and emit one final frame per page.
- ❌ **Don't rely on `MouseArea`/synthesized mouse alone** — Qt6's synth-mouse path can stall and gives no multi-touch;
  always have the real `QTouchEvent` path *and* the mouse fallback.
- ❌ **Don't forget `setAcceptTouchEvents(true)`** — the #1 reason `touchEvent()` silently never fires in Qt6.
- ❌ **Don't full-flash for every small UI change** — reserve the ~1 s colour flash for page turns / new pages; use tight
  mono partials elsewhere. And **don't let partials cover most of the screen** (they get promoted to flashes).
- ❌ **Don't let finger touches turn pages while the pen is active** — biggest source of accidental turns; gate touch on
  pen proximity.
- ❌ **Don't `EVIOCGRAB` the input devices** — open shared so the QPA/system keep working (research-reuse.md §"Input").

---

## Sources
- KOReader defaults (tap zones, overlap): https://github.com/koreader/koreader/blob/master/defaults.lua
- KOReader gesture detector (thresholds, swipe/tap/hold/pan): https://github.com/koreader/koreader/blob/master/frontend/device/gesturedetector.lua
- KOReader paging module (page turn, overlap, scroll activation): https://github.com/koreader/koreader/blob/master/frontend/apps/reader/modules/readerpaging.lua
- KOReader tap-interval/debounce PRs: https://github.com/koreader/koreader/pull/6798 · https://github.com/koreader/koreader/pull/5138
- KOReader e-ink refresh (FULL_REFRESH_COUNT, flash on images): https://koreader.rocks/user_guide/ · https://github.com/koreader/koreader/issues/10047 · https://github.com/koreader/koreader/issues/13863
- KOReader gesture customization guide: https://www.ereadersforum.com/threads/how-to-customize-page-turns-and-gestures-in-koreader-on-any-e-reader.7807/
- Plato manual (9-region tap zones, swipes, refresh gesture): https://github.com/baskerville/plato/blob/master/doc/MANUAL.md
- netsurf-reMarkable (input/evdev, chrome reveal): https://github.com/alex0809/netsurf-reMarkable · https://github.com/alex0809/netsurf-reMarkable/blob/main/README.md
- Kindle EasyReach tap zones: https://thomaspark.co/2012/01/kindle-touch-gestures/
- Kobo page-turn taps/swipes + accidental-turn prevention: https://help.kobo.com/hc/en-us/articles/360017481194-Turn-pages-on-your-eReader · https://help.kobo.com/hc/en-us/articles/21346752946839-Prevent-accidental-page-turns-while-reading
- e-ink ghosting/full-refresh explainer: https://www.paperlessmode.com/how-to-fix-e-ink-ghosting-burn-in/
- Qt6 input events (handlers over MouseArea, synthesized-event detection, QPA device registration): https://www.qt.io/blog/input-events-in-qt-6
- QQuickItem (setAcceptTouchEvents required in Qt6): https://doc.qt.io/qt-6/qquickitem.html
- QTouchEvent (must accept TouchBegin): https://doc.qt.io/qt-6/qtouchevent.html
- TapHandler (recommended over MouseArea): https://doc.qt.io/qt-6/qml-qtquick-taphandler.html
- Custom QQuickPaintedItem touch (setAcceptedMouseButtons fix): https://forum.qt.io/topic/79364/touch-gesture-recognition-with-a-custom-qquickpainteditem
- Qt synthesized-mouse-stops bug: https://forum.qt.io/topic/96780/qt-widgets-not-responding-to-touch-event-sometimes
- Qt embedded inputs / evdevtouch / QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS: https://doc.qt.io/qt-6/inputs-linux-device.html
- reMarkable official Qt Quick / epaper QPA guide (touch out-of-box, MouseArea sample, Screen sizing): https://developer.remarkable.com/documentation/qt_epaper
- Cross-references (this repo): docs/research-reuse.md §1 (refresh facts), §2/§2a (rMPP refresh API + control loop), §"Input" (event-node map, transforms, shared-open); docs/device-profile.md (hardware)
