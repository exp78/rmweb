# Browser UI survey — what to borrow for rmweb

> Cross-project survey (2026-06-28) of open-source browsers, done to design rmweb's
> reading shell. rmweb stack reminder: chrome = **hand-painted C++ (QQuickPaintedItem — QuickControls2
> is not linked)**,
> engine = **WPE WebKit on CPU** (renders ARGB frames), display = **color e-ink** (Gallery 3,
> 1620×2160, slow refresh), input = **finger** (pen is not handled). MVP = a calm *reading* browser.
>
> Method: four parallel research agents over four clusters. Every claim below traces to a
> repo cited in **Sources**. This doc is the "насмотренность" map — what each project does
> and exactly what we lift vs. skip.

---

## 0. The one load-bearing finding — the "WebView façade" seam

Every QML browser (Angelfish, Sailfish, Liri) converges on the **same architecture**:
the web engine is wrapped in **ONE QML item** that re-exposes a small, stable
property/method surface, and **all chrome binds only to that wrapper**. The engine is
therefore swappable without touching the chrome.

**→ rmweb action:** define our WPE-backed QML item to expose **Angelfish's façade names
verbatim**, so borrowed chrome binds with zero renaming. We already have `WpeEngine` +
`ShellBridge`; reshape the exposed API to this contract:

```
properties:  url, title, icon, loading, loadProgress, canGoBack, canGoForward,
             requestedUrl, readerMode, findCount, findIndex, privateMode
methods:     goBack(), goForward(), reload(), stop(), loadUrl(u),
             findText(s), findNext(), findPrev(), readerToggle()
signals:     urlChanged, titleChanged, loadProgressChanged, navStateChanged,
             tlsStateChanged, processCrashed
```

Reference file to copy names from: Angelfish `lib/contents/ui/WebView.qml`.

---

## 1. Engine ↔ chrome signal wiring (reference: GNOME Web / Epiphany)

Epiphany (`embed/ephy-web-view.c`, a `WebKitWebView` subclass) is the canonical map of
**which WebKit/WPE signal drives which piece of chrome**. We use the *same* WebKitWebView
API, so this is a direct spec for the WPE→QML bridge:

| WebKit signal / call | Chrome effect |
|---|---|
| `load-changed` (STARTED/COMMITTED/FINISHED) + `notify::estimated-load-progress` | progress line |
| `notify::title`, `notify::uri` | toolbar title + address field |
| `notify::is-loading` | reload ⇄ stop button swap |
| `can_go_back` / `can_go_forward` | enable/disable back/fwd |
| `load-failed-with-tls-errors` + `webkit_web_view_get_tls_info()` | security/lock indicator |
| `web-process-terminated` | **error page + auto-reload** (mitigates our "segfault reboots device" risk at the chrome layer) |
| `webkit_web_view_get_find_controller()` → `counted-matches` | find-in-page "3/12" |
| `webkit_web_view_set_zoom_level()` | text zoom |
| `WebKitUserContentManager` + `WebKitUserContentFilter` (JSON ruleset) | content/ad blocking (we already do this) |

cog and MiniBrowser-WPE add the **resilience + config** patterns: cog's
`--webprocess-failure=restart` policy, and MiniBrowser's CLI flag set
(`--content-filter`, `--enable-itp`, `--cookies-policy=no-third-party`, `--proxy`,
`--ignore-tls-errors`, `--bg-color`, `--size`) is a ready-made **settings schema** for us.
Neither has real chrome (both are kiosk/minimal) — take their *engine wiring & flags*, not UI.

---

## 2. The e-ink interaction model (KOReader · EinkBro · Plato · Kindle)

This is the core of the reading UX. The consensus across every dedicated e-ink reader:

- **Paginate, don't scroll.** Page-turn = jump **one viewport-height** (or CSS columns).
  Smooth pixel-scroll smears/ghosts on e-ink. This is the single biggest reading idea.
- **Asymmetric invisible tap-zones** (Kindle EasyReach + KOReader's 25/75 default + Plato edges):
  - right ~75% / full-width swipe-left → **next page**
  - narrow left ~25% → **prev page**
  - top strip → summon chrome
  - tap-and-hold → select (never turns)
  - **flat single-frame turn, no animation** (Kindle/EinkBro rule).
  We already read finger evdev (`TouchReader`) and emit swipes — extend it to tap-zones.
- **Adaptive refresh state machine** — *rmweb uniquely owns the waveform* via the epaper QPA
  (`EPFrameBuffer::setForceFull`). EinkBro/NetSurf structurally cannot do this; it's our edge.
  - **fast / grayscale (A2-like)** while a swipe is in flight → coalesce deltas, present *latest frame only*.
  - **Regal/GL16-like** for the settled page (low-flash, no ghost).
  - **full GC16** on: navigation, color/image entering view, reader toggle, every N turns, and a ~1–2 s **idle ghost-clear**.
  - Settings: "full refresh every N pages" (`never` / `every N` / `by chapter`), a manual
    "full refresh now" gesture, and a **Grayscale reading mode** toggle (kills the color
    flash app-wide). Ties to our existing `RMWEB_FULL_EVERY`.
- **Volume/edge/FAB all map to next/prev** (EinkBro). We have a pen + finger; map a pen
  gesture or an on-screen edge.
- **No animations, no spinners, no smooth-scroll, no dialog dim-masks** (EinkBro's whole
  thesis: fewer repaints + smaller repaint areas). Loading = static text or a stepped
  determinate bar, never a spinner. Menus = instant swap.

KOReader refresh defaults worth copying: partial per turn, full **by chapter** (community
favourite) or every ~6–20 turns; pages with images force a full flash (toggleable).
**NetSurf-reMarkable** is archived/near-zero-JS — it validates our WPE choice but we take
**nothing** from it (its refresh internals are undocumented; KOReader is the playbook).

---

## 3. Reader mode — the single highest-value feature (Mozilla Readability.js)

A declutter-to-clean-article mode is the killer feature for slow color e-ink. Use
**Mozilla Readability** (Apache-2.0, zero deps, the lib behind Firefox Reader View) injected
into our live WPE DOM. Runs fine in the JSC **interpreter** (no JIT needed).

**Integration recipe (native C on the WebKitWebView + injected JS):**
1. Bundle `Readability.js` + `Readability-readerable.js` in `/home/root/rmweb`.
2. Inject `Readability-readerable.js` as a user script at document-end via
   `webkit_user_content_manager_add_script(...)`.
3. Detector → native: register a script-message handler **before** registering the name
   (documented race); injected glue posts
   `isProbablyReaderable(document)` → QML lights the reader toggle.
4. On tap: `webkit_web_view_evaluate_javascript("(function(){var c=document.cloneNode(true);"
   "var a=new Readability(c).parse();return a?JSON.stringify(a):null})()")`.
   **Gotcha:** `parse()` mutates the DOM → always `cloneNode(true)` first.
5. **Render (Strategy A, recommended):** build a fresh minimal HTML doc with *our own e-ink
   reader CSS* and `webkit_web_view_load_html(html, original_uri)` (pass the original URL as
   `base_uri` so relative `<img>` resolve). White bg, device fonts only → fewest full
   refreshes, tiny DOM → fastest llvmpipe paint. Track a `readerMode` flag + stored URL
   yourself (load_html replaces history); "exit reader" = reload original. (Later: an
   `rmweb:reader?url=` scheme handler for a real back-stack, like Firefox `about:reader`.)

**Reader toolbar (each control = one CSS var, re-render = one full refresh, persist choices):**
font size ±, **serif / sans** (device fonts), **content width** (~60–85ch — big lever on the
wide panel), line spacing (1.3/1.5/1.8), **bold/weight** (contrast boost). Theme = **Light
(white) + High-Contrast** only — *no sepia/gray bg* (pointless on grayscale, triggers costly
color refresh). Minimum-viable = size ± / serif-sans / width / line-spacing (~90% of value).
**Fix Kindle's flaw:** paginate the *whole* article, never force-exit to advance.

---

## 4. QML chrome to lift (Angelfish ★, Liri, Sailfish)

- **Angelfish** (Kirigami + Controls2, GPL-3) — most relevant.
  - `src/qml/Navigation.qml` = the toolbar as a **Controls2 `RowLayout` of `ToolButton`**
    (back/fwd/reload-stop/address/menu/tabs) reading `canGoBack/canGoForward/loading/requestedUrl`
    — **near-verbatim into our existing ToolBar** (it's Controls2, not Kirigami here).
  - `InputSheet.qml` + `UrlDelegate.qml` = URL-entry overlay + suggestion row (pairs with our
    URL field + our own C++ on-screen keyboard).
  - `FindInPageBar.qml`, bookmarks/history/downloads pages, settings-as-page-stack
    (`src/settings/*`: General / Adblock / SearchEngine / NavigationBar).
  - Web-prompt dialogs: `PermissionQuestion`, `AuthSheet`, `JavaScriptDialogSheet`.
  - **Reimplement, don't reuse:** Kirigami `ApplicationWindow`/`GlobalDrawer`/`OverlaySheet`
    → Controls2 `ApplicationWindow` + `Drawer`/`Popup` + `StackView`. Drop haptics.
- **Liri Browser** (plain Controls2 + Material, GPL-3) — closest *component vocabulary* to us.
  - Lift `BrowserToolbar.qml`, `Omnibox.qml` + `SearchSuggestions.qml`, `LoadingIndicator.qml`,
    the **`*Themed.qml` global-skin pattern** (one place to impose an e-ink high-contrast theme),
    the swappable `BrowserWebView.qml` wrapper. Strip Material (ripples/shadows = bad on e-ink).
- **Sailfish Browser** (Silica, MPL-2) — *patterns only* (Silica isn't portable).
  - `Overlay.qml` + `OverlayAnimator.qml` = chrome-slides-over-content show/hide model.
    Adopt as **tap-to-toggle** reading chrome (instant show/hide, drop the slide animation).
  - Tab-grid switcher decomposition, favorites-as-grid, permission-exceptions taxonomy.
- **Falkon** (QtWidgets, GPL-3) — **feature/settings taxonomy only**, zero UI code: the
  Phase-6 "full browser" backlog (sessions, profiles, password mgr, RSS, GreaseMonkey, sidebar).

---

## 5. Link hinting for imprecise touch (qutebrowser ★)

e-ink touch is imprecise; dense link lists are unfair to fingers. **Strongly recommended as a
primary nav mode**, not a power-user extra. Borrow qutebrowser's algorithm
(`browser/hints.py`): overlay short labels on links/buttons/inputs, label length
`ceil(log_alphabet(n))` with a min, **word-mode labels** (most readable on e-ink), **scatter**
so first chars disambiguate, **type-to-filter / tap-the-label to follow**. Touch adaptation:
make the label a **large tappable badge** (tap *or* type). Trigger from one toolbar button or a
pen gesture. (luakit/Nyxt/vimb are the same idea; qutebrowser's is the cleanest spec.)

Also from the minimal cluster: **Nyxt's single fuzzy prompt** as a universal entry point
(open/search/command/switch-page) collapses chrome into one overlay — good fit for a low-button
e-ink UI. **vimb's `:queue`** = read-it-later. **badwolf/surf** = JS-off-by-default + per-tab
ephemeral + single-key "kill JS/images" escape hatch for heavy pages on CPU.

---

## 6. Feature tiers (MVP / v1 / later)

| Feature | Tier | e-ink + touch note |
|---|---|---|
| Façade refactor + Epiphany signal wiring | **MVP** | the seam everything else binds to |
| Smart bar (URL+search, one field) + autocomplete from history/bookmarks | **MVP** | full-screen overlay, big target, on-screen keyboard; debounce, partial refresh |
| Back/Fwd/Reload-Stop/Home | **MVP** | edge-swipe back + buttons; reload⇄stop combined |
| **Paginate + asymmetric tap-zones + flat turns** | **MVP** | next=right/swipe, prev=left strip, top=chrome |
| **Adaptive refresh + "full every N" + grayscale-mode toggle** | **MVP** | we own the waveform — our differentiator |
| **Reader mode (Readability.js)** | **MVP** | marquee feature; Strategy A render; reader toolbar |
| Reading list / save-for-later + curated start page | **MVP** | core for a reading device; start page doubles as home |
| Content/JS blocking ON by default + JS toggle | **MVP** | +70% speed on heavy pages (Kobo data); we have blocking |
| Security/TLS indicator + web-process-crash recovery | **MVP** | lock glyph; error page + auto-reload |
| **Link hinting** | MVP-lite → **v1** | qutebrowser algorithm; tap-or-type labels |
| Find-in-page | v1 | overlay + next/prev, jump-scroll one partial refresh |
| Bookmarks / History pages | v1 | reuse smart-bar overlay list; SQLite |
| Settings page | v1 (tiny in MVP) | flat list of toggles, no nested panels |
| Search-engine keywords (`g `, `w `) | v1 | prefix tokens in smart bar |
| Private / ephemeral mode | v1 | a toggle, not a window |
| Tabs (as fuzzy page-switcher, ≤3, suspend rest) | later | **no visible tab strip**; Nyxt-buffer model |
| Downloads (flat list) | later | save to `/home/root` |
| Share / export → PDF / ePub | later | reader-mode article → file (reMarkable doc model) |

**Recommendation — tabs:** **single-page + a fuzzy "open pages" overlay**, not a visible tab
bar (persistent chrome costs area + refreshes; CPU-only WebProcesses are expensive; reading
device rarely juggles pages). Cap live pages ≤3 later, suspend the rest.

---

## 7. Explicitly DO NOT build (YAGNI for a focused e-ink reader)

Visible multi-tab bar · vim modal system as the *primary* UX · any animation / smooth-scroll /
spinner / hover / theme transition · extensions / WebExtensions / DevTools UI · sync / accounts
/ password manager · media player chrome / autoplay / WebRTC / PiP / WebGL (no GPU driver in the stock OS) ·
notifications / PWA install · download manager beyond a flat list · live new-tab feed / widgets ·
sepia or colored reader backgrounds · large solid-black toolbars/headers (worst ghosting) ·
autocomplete that re-renders every keystroke · favicons-everywhere color UI (monochrome-first;
color only for content that needs the Gallery-3 full refresh).

---

## Sources

- **WPE/WebKit:** cog https://github.com/Igalia/cog · WebKit MiniBrowser
  https://github.com/WebKit/WebKit/tree/main/Tools/MiniBrowser (wpe/ + gtk/) ·
  Epiphany https://gitlab.gnome.org/GNOME/epiphany
- **QML touch browsers:** Angelfish https://invent.kde.org/network/angelfish ·
  Sailfish Browser https://github.com/sailfishos/sailfish-browser ·
  Falkon https://github.com/KDE/falkon · Liri https://github.com/liri-project/liri-browser
- **E-ink / reader:** EinkBro https://github.com/plateaukao/einkbro ·
  KOReader https://github.com/koreader/koreader ·
  netsurf-reMarkable https://github.com/alex0809/netsurf-reMarkable (archived) ·
  Plato https://github.com/baskerville/plato ·
  Mozilla Readability https://github.com/mozilla/readability
- **Minimal / keyboard:** surf https://git.suckless.org/surf · vimb https://github.com/fanglingsu/vimb ·
  badwolf https://hacktivis.me/projects/badwolf · qutebrowser https://github.com/qutebrowser/qutebrowser ·
  luakit https://github.com/luakit/luakit · Nyxt https://github.com/atlas-engineer/nyxt
