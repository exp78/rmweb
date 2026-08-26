# rmweb Reading-Shell MVP — Design Spec

**Goal:** Turn the working WPE-on-e-ink engine into a calm, complete *reading* browser:
fullscreen reading with summonable chrome, page-turn navigation, an adaptive e-ink refresh
state machine we control, a Readability.js reader mode, a smart address bar with history/
bookmark autocomplete, a reading list + start page, content/JS blocking, an e-ink theme, and
lite link-hinting for imprecise touch.

**Architecture:** Keep the existing 5-module split (`engine` · `display` · `input` · `shell` ·
`platform`). The web engine is wrapped behind ONE QML façade (Angelfish naming) that all chrome
binds to; the engine stays swappable. New behavior is added as small, mostly-pure helpers
(host-testable) plus QML chrome lifted from Angelfish/Liri (Controls 2, no Kirigami/Material).

**Tech stack:** WPE WebKit 2.48 (CPU/Skia, JSC interpreter), Qt6 Qt Quick Controls 2 (epaper QPA),
Mesa llvmpipe, libsoup3, SQLite3, Mozilla Readability.js (Apache-2.0, bundled). C/C++ cross-built
via the ferrari Yocto SDK; pure-logic unit tests built on host with clang++.

Source of borrowed patterns + citations: `docs/research/browser-ui-survey.md`.
Verified device facts this builds on: `CLAUDE.md`, `docs/research/remarkable-touch-input.md`,
`docs/research/wpe-rendering-protocol.md`, and the project memories
(`eink-async-frame-display-bug`, `jit-works-polling-traps`, `six-second-render-softpipe`).

---

## Global Constraints (inherited by every task)

- **No usable GPU: the SoC has one (Vivante), but the stock OS ships no driver for it** → no
  EGL/GLES. Software GL only (Mesa llvmpipe, surfaceless EGL); page paint = Skia CPU.
- **Install ONLY under `/home/root/rmweb`** (rootfs `/` is full); bundle missing libs, set rpath.
- **Cross-compile only** via the ferrari SDK (scarthgap, glibc 2.39, aarch64, `-mcpu=cortex-a53`).
- **Display = Qt6 epaper QPA**, QtQuick **only**, `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`,
  Window sized to `Screen.width/height`, **xochitl stopped on run / restored on exit**.
- **Present serialization is mandatory** (see §4): the vendor epaper present DEADLOCKS if a 2nd
  present overlaps the 1st. Never let two presents overlap.
- **JSC interpreter by default** (`JSC_useJIT=0`); JIT optional via `RMWEB_JIT=1`
  (+`JSC_usePollingTraps=1`). Readability runs fine in the interpreter — do **not** depend on JIT.
- **A process segfault reboots the device** (~100 s watchdog). Keep the `-rdynamic` SIGSEGV
  backtrace handler; write logs under `/home/root` so they survive a reboot.
- **Respond to the user in Russian.** Per-phase: implement → verify on device → code-review
  subagent → simplify subagent. Commit trailer `Co-Authored-By: Claude Opus 4.8 …`.
- **`.env` (REMARKABLE_PASSWORD) and `build/` are gitignored and MUST NEVER be committed.**
  Every commit guards: `if git check-ignore -q .env; then commit; else ABORT`.

---

## 1. The engine façade (the seam everything binds to)

`WpeEngine` (worker thread) + `ShellBridge` (GUI-thread proxy exposed to QML as `engine`) are
reshaped to expose the **Angelfish `WebView.qml` contract** so borrowed chrome binds unchanged:

```
properties (read-only to QML, updated via signals):
  url, title, icon, loading, loadProgress (0..1), canGoBack, canGoForward,
  requestedUrl, readerMode (bool), readerable (bool), findCount, findIndex,
  jsEnabled (bool), tlsOk (bool), tlsHost (string)
methods (invoked from QML, marshalled to the engine GMainContext):
  loadUrl(string), goBack(), goForward(), reload(), stop(),
  pageNext(), pagePrev(),                  // pagination (see §3)
  readerToggle(), setReaderStyle(obj),     // reader mode (see §5)
  findText(string), findNext(), findPrev(), findClear(),
  setJsEnabled(bool), hintStart(), hintFollow(string)
signals:
  urlChanged, titleChanged, loadProgressChanged, navStateChanged,
  readerableChanged, readerModeChanged, tlsStateChanged,
  processCrashed, findResultChanged, hintsReady(list)
```

Wiring follows the Epiphany map (`docs/research/browser-ui-survey.md` §1), driven by the same
WebKitWebView API we already use: `load-changed` + `notify::estimated-load-progress` →
`loading`/`loadProgress`; `notify::title`/`notify::uri` → `title`/`url`; `is-loading` → reload⇄stop;
`can_go_back`/`can_go_forward` → `navStateChanged`; `load-failed-with-tls-errors` +
`webkit_web_view_get_tls_info()` → `tlsStateChanged`; `web-process-terminated` → `processCrashed`.

**Cross-thread rule (already established):** QML ↔ engine only via `ShellBridge` on the GUI thread;
engine work marshalled with `g_timeout_source_new` + `g_source_attach(m_ctx)` (NOT the default
context). Frames cross as BGRA `QImage` (BGRA == ARGB32).

---

## 2. Reading-first chrome (layout: approved "reader-first")

QML (`shell`), Controls 2 only, lifted from Angelfish `Navigation.qml` + `InputSheet.qml` +
`UrlDelegate.qml`, reskinned via Liri's `*Themed.qml` global-skin pattern.

- **Default = fullscreen content, zero chrome.** The WebView item fills the Window.
- **Summon:** a tap in the **top strip** (or a two-finger tap anywhere) toggles chrome:
  - **Top bar:** `‹ ›` back/fwd · reload/stop (combined) · address affordance (tapping opens the
    address overlay) · reader toggle (visible only when `readerable`) · `☰` menu.
  - **Bottom bar:** determinate progress + page/percentage indicator + reading-list `📑` add.
  - Chrome appears/disappears as an **instant swap** (no slide animation — e-ink). Tapping page
    content (outside the top strip) dismisses chrome.
- **Address overlay** (`InputSheet`): full-screen panel, large `TextField` + Qt Virtual Keyboard +
  an autocomplete list (`UrlDelegate` rows) from history + bookmarks. One field = URL or search
  (heuristic: contains a dot or a scheme → URL via `rmweb::normalizeUrl`; else search query).
- **Menu (`☰`):** reading list / start page (home) · bookmarks add/open · settings · JS toggle ·
  "full refresh now" · grayscale-mode toggle · link-hint mode.
- **Theme:** high-contrast monochrome Controls 2 — black on white, no gradients, no large solid-
  black fills (worst ghosting), no shadows/ripples. One `Theme.qml`/`*Themed.qml` skin layer.

---

## 3. Page-turn navigation (paginate, don't scroll)

Reading uses **discrete pagination**, never smooth scroll (smears/ghosts on e-ink).

- **`pageNext()`/`pagePrev()`** = scroll the view by ~one viewport height (minus a small overlap),
  clamped to document bounds. **Verified gotcha (`wpe-rendering-protocol.md`):** a bare `scrollBy`
  changes `scrollY` but emits **no buffer** — a tiny DOM mutation must follow to force an immediate
  repaint (`flip-latency ≈ 23 ms`). `pageNext/Prev` therefore = scrollBy + forced repaint, then
  present one final frame.
- **Tap-zones (invisible), classified by the pure `tapzone.h`:** left edge (~22%) → `pagePrev`;
  right edge (~22%) → `pageNext`; **center band → pass the tap through to content** (follow links
  via the touch→mouse bridge); top strip (~8%) → summon chrome. A full-width **swipe** turns pages
  regardless of zone (swipe-up → next, swipe-down → prev, per the existing `gesture.h`). Edge-zones
  (not half-screen) keep the center tappable for links — which matters for a browser vs a pure
  e-reader; dense/edge links are covered by link-hint mode (Phase D). Zone fractions configurable.
  This **extends the existing `TouchReader`** (event3 finger, Protocol-B, `EVIOCGRAB`, maps
  `x*1620/2064`, `y*2160/2832`) which already debounces and emits swipes.
- At document end, `pageNext` is a no-op (no wrap). Page indicator = `scrollY / scrollHeight`.

---

## 4. Adaptive e-ink refresh controller (our differentiator)

We own the waveform via the epaper QPA — EinkBro/NetSurf cannot. The controller lives in the
`display` layer around `WpeView`'s present; the *policy* is the pure, host-tested `refreshpolicy.h`.

- **Present serializer (correctness, mandatory):** at most ONE present in flight; new frames
  coalesce to the **latest** (drop intermediate). The current fix uses a fixed `kPresentGapMs=2000`
  proxy for "previous present finished" — **replace it with completion-gated serialization**: gate
  the next present on actual panel-refresh completion (epaper completion signal if exposed, else a
  waveform-aware timeout — fast/grayscale ≈ 150 ms, full color ≈ 1–1.5 s). This both keeps the
  deadlock impossible AND unlocks the measured ~120–250 ms page turns (the 2 s gap is the only
  thing making turns feel slow today).
- **Waveform policy (`refreshpolicy.h`):** input `(turnCount, isNavigation, hasColorContent,
  idleMs, userGrayscaleMode)` → output `{Fast | Full}` + whether to reset the turn counter.
  - **Fast (grayscale)** for the settled reading page and in-flight motion.
  - **Full (GC16, color flash)** on: navigation (new URL), reader toggle, every **N** turns
    (setting: `never` / `every N` / default ≈ chapter-like 10–20), color/image entering view, and a
    **~1–2 s idle ghost-clear** (promote the last fast frame to a clean full).
  - **Grayscale reading mode** (setting) forces Fast app-wide → no color flash, max responsiveness.
- **Manual "full refresh now"** (menu + a reserved gesture) → one GC16.
- Ties to the existing `RMWEB_FULL_EVERY` knob (now a user setting, not just an env var).

---

## 5. Reader mode (Readability.js — the marquee feature)

Native C on the WebKitWebView + injected JS; runs in the JSC interpreter (no JIT).

- **Bundle** `Readability.js` + `Readability-readerable.js` under `/home/root/rmweb` (`platform`,
  via `scripts/bundle.sh`).
- **Detect:** inject `Readability-readerable.js` as a user script at document-end; injected glue
  posts `isProbablyReaderable(document)` over a script-message handler (register the handler name
  **after** connecting `script-message-received::readerProbe` — documented race) → sets `readerable`
  → top bar shows the reader toggle.
- **Enter reader (`readerToggle`):** `evaluate_javascript` of
  `(function(){var c=document.cloneNode(true);var a=new Readability(c).parse();return a?JSON.stringify(a):null})()`
  (**clone first — `parse()` mutates the DOM**) → `{title, byline, content, …}`.
- **Render — Strategy A:** build a fresh minimal HTML doc with rmweb's own e-ink reader CSS and
  `webkit_web_view_load_html(html, original_uri)` (pass original URL as `base_uri` so relative
  `<img>` resolve). White bg, device fonts only → fewest full refreshes, tiny DOM → fastest paint.
  Track `readerMode` + stored original URL; **exit reader = reload original**. Paginate the whole
  article (fix Kindle's forced-exit flaw).
- **Reader toolbar (`setReaderStyle`)** — each control = one CSS var in the template, persisted:
  font size ±, **serif / sans** (device fonts), content width (~60–85ch), line spacing
  (1.3/1.5/1.8), **bold/weight** (contrast). Theme = Light (white) / High-Contrast only — **no
  sepia/gray bg**.

---

## 6. Persistence & start page (`platform`)

- **SQLite3** (already a bundled dep) for: `history(url, title, ts, visit_count)`,
  `bookmarks(url, title, ts)`, `reading_list(url, title, ts)`. Simple DAO exposed to QML as list
  models for the address autocomplete + start page + reading list.
- **Reading list (save-for-later)** is core for a reading device: top-bar `📑` adds the current
  page (URL + title). MVP stores the reference only; offline snapshot (MHTML/single-file) is later.
- **Start page** = the home/new-tab: a static, mostly-monochrome grid of bookmarks + reading list +
  a few default destinations. No feeds, no animation. `Home` opens it.
- **Settings** persisted: default search engine, JS on/off, refresh-every-N, grayscale mode,
  reader style, theme. Plain flat list of toggles (no nested panels).

---

## 7. Link-hinting (lite) for imprecise touch (`input`/`shell` + engine)

- **`hintStart()`** injects JS that overlays short **tappable badge labels** on links/buttons/inputs
  and posts the label↔target map back (`hintsReady`). Labels from the pure `hintlabels.h`:
  alphabet from a configurable charset, length `ceil(log_alphabet(n))` with a min-chars floor,
  **word-mode** preferred (most readable on e-ink), scatter to disambiguate first chars.
- **Follow:** tap a badge **or** type the label → `hintFollow(label)` navigates. `Esc`/tap-away
  cancels. Triggered from the menu or a reserved gesture.

---

## 8. Content/JS control (already present, extended)

- Content-blocking ON by default via `WebKitUserContentManager` + `WKContentRuleList` (compiled
  JSON ruleset) — drop third-party scripts/ads/media/fonts (lightens CPU-bound pages; Kobo data:
  ~+70% speed). Settable off.
- **JS enable/disable toggle** (`setJsEnabled`) — one-tap escape hatch for heavy pages
  (surf/badwolf precedent), via `WebKitSettings:enable-javascript`.

---

## 9. Error handling & resilience

- **WebProcess crash:** `web-process-terminated` → `processCrashed` → shell shows a simple error
  page + auto-reload with a **bounded retry budget** (guard a reload-loop with an attempt counter +
  backoff).
- **TLS errors:** `load-failed-with-tls-errors` + `get_tls_info()` → `tlsStateChanged`; top bar shows
  a broken-lock indicator. MVP indicates only; "proceed anyway" is v1.
- **Segfault safety:** keep `-rdynamic` + the SIGSEGV backtrace handler; logs to `/home/root`
  (survive the watchdog reboot). Avoid the software scenegraph (proven to crash); stay on epaper.
- **Present deadlock:** structurally impossible under the §4 serializer.

---

## 10. Module / file map (for the plan)

- **`engine`** (C, `engine/wpeqt/`): reshape `WpeEngine`/`ShellBridge` to the §1 façade; new
  `reader.{c,h}` (Readability inject/detect/parse/render); `pagination` (scrollBy + forced repaint);
  `hints` (inject overlay, follow); JS-toggle + content-filter hook (extend existing `m_ucm`).
- **`display`** (C++/QML around `WpeView`): present serializer (completion-gated) + refresh
  controller consuming `refreshpolicy.h`.
- **`input`** (`TouchReader`): extend to classify tap-zones via `tapzone.h` (keep swipes).
- **`shell`** (QML): reader-first `main.qml`; `Navigation` bar; address `InputSheet` + `UrlDelegate`
  autocomplete; `ReaderToolbar`; `StartPage`; `Settings`; `Menu`; `Theme`/`*Themed` e-ink skin.
- **`platform`**: SQLite DAO + QML list models; bundle Readability.js; settings store.
- **Pure logic + host tests** (`tests/`, clang++, like existing `gesture.h`/`url.h`):
  `tapzone.h` (xy→action), `hintlabels.h` (N→labels), `refreshpolicy.h` (state→waveform).

---

## 11. Data flow

`evdev (event3)` → `TouchReader` + `tapzone.h` → action → `ShellBridge` (GUI) → `engine` method
(marshalled to GMainContext) → WebKit renders BGRA → `WpeView` present serializer + `refreshpolicy`
→ epaper QPA → e-ink. Engine state changes → signals → `ShellBridge` → QML chrome updates.

---

## 12. Testing strategy

- **Host unit tests** (clang++, no device): `tapzone`, `hintlabels`, `refreshpolicy`, plus existing
  `gesture`, `url`. TDD: failing test → minimal impl → pass → commit.
- **Device verification** per phase (working agreement): real pages render, page turns land on
  e-ink, reader mode declutters a real article, autocomplete/reading-list/start-page work, crash
  recovery shows the error page, refresh feels fast (turns < ~300 ms, full-flash only when policy
  says). Verify with the user's eyes / a captured frame, not engine logs alone.
- After each phase: code-review subagent → simplify subagent.

---

## 13. Out of scope (v1 / later — do NOT build now)

Visible multi-tab bar (use single-page + a later fuzzy "open pages" overlay, ≤3 live, suspend rest);
find-in-page UI (façade has the hooks, chrome is v1); dedicated bookmarks/history pages (MVP reuses
the address overlay list + start page); search-engine keywords; private/ephemeral mode; downloads
manager; PDF/ePub export; "proceed through TLS error"; selection/copy-paste; extensions/DevTools/
sync/accounts/password manager; media/WebRTC/WebGL/autoplay/PiP/notifications/PWA; animations/
smooth-scroll/spinners; sepia/colored reader backgrounds; live start-page feeds/widgets.

---

## 14. Risks / open questions

- **Present-gap tuning** is the make-or-break for "fast" turns: must move from the fixed 2 s proxy
  to completion-gated serialization without reintroducing the overlap deadlock. Highest-risk task;
  verify carefully on device.
- **Pagination fidelity:** scrollBy + forced-repaint must land on clean page boundaries (overlap
  tuning); some sites with sticky headers/odd layouts may mis-paginate — acceptable for MVP.
- **Reader coverage:** Readability fails on some pages (`parse()` → null) → keep the toggle disabled
  (`readerable=false`); not all sites get reader mode (expected).
- **Heavy first-party SPAs** remain slow even with JIT/blocking — acknowledged, out of scope.
