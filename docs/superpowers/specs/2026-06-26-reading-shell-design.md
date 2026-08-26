# rmweb — reading shell (chrome + navigation + URL entry)

**Status:** approved design (2026-06-26) · **Parent:** [`2026-06-24-rmweb-browser-design.md`](2026-06-24-rmweb-browser-design.md) · **Phase:** 4 (scope A)
**Builds on:** the working WPE→Qt→epaper engine (Phase 3) and llvmpipe fast page-turns (Phase 4).

## 1. Purpose

Wrap the existing full-screen WPE web view in a minimal **reading-browser shell**: a top toolbar
(back / forward / reload + an address field), real URL navigation, and on-screen URL entry. The
guiding rule from the user: **reuse everything possible — don't reinvent the wheel.** Every piece
below is an existing component (Qt Quick Controls, WebKit's own navigation API, the Qt Virtual
Keyboard module, our existing `TouchReader`/`EpaperRefresh`).

## 2. What already works (verified on device, 2026-06-26)

- **Real HTTPS pages render to e-ink.** `https://example.com` loaded over TLS (glib-networking /
  openssl) and painted two frames (`load finished @575ms`, `frame 1/2 NEW`). Full path live:
  Wi-Fi → DNS → TLS → WPE WebKit (Skia CPU) → llvmpipe → epaper QPA → e-ink.
- **Internet is up** on the device: `wlan0` default route, ping + DNS OK.
- **Qt Quick Controls present on device** (`/usr/lib/qml/QtQuick/Controls`, styles Basic / Fusion /
  Material / Imagine / Universal). Qt is **6.8.2**.
- **No on-screen keyboard on device** when `xochitl` is stopped: Qt Virtual Keyboard is **not**
  installed, and the only "keyboard" assets are for the **physical** Type Folio
  (`libqevdevkeyboardplugin.so`, `rMkeyboard01_fw_*`). reMarkable's own OSK lives inside `xochitl`,
  which we stop. → we must **bundle** Qt Virtual Keyboard ourselves.
- **Current input** (`engine/wpeqt/main.cpp`): `TouchReader` `EVIOCGRAB`s the finger digitizer
  (node "Elan touch input" = **event3**; note the original device-profile mapping was backwards —
  event3 = touch, event2 = pen), decodes Protocol-B, emits page-turn swipes. The grab is also what
  silences the epaper QPA's broken touch dispatch (it posts touch with a NULL window → Qt drops it →
  WebKit crash). So **we already own all finger touch.**

## 3. Key decisions

### 3.1 Keystone: a **touch → mouse bridge**

Because we already exclusively grab the finger digitizer, the cheapest way to make *all* chrome
interactive is to feed Qt synthetic mouse events instead of hand-coding hit-testing. Extend
`TouchReader` to classify each contact:

- **Swipe** (vertical travel ≥ `kSwipeMinDy`, roughly vertical) → page turn (unchanged).
- **Tap** (down→up, small movement, short dwell) → emit `tap(x, y)` in panel pixels.

A tap is turned into a `QMouseEvent` **press+release at (x, y)** delivered to the `QQuickWindow` on
the **GUI thread**. Qt Quick then runs its own scene hit-testing and routes the click to whatever
Control sits under the point — a toolbar button, the address `TextField`, or a virtual-keyboard key.

Consequence: **every Qt Quick Control works through Qt's native delivery, with zero bespoke widget
logic.** The same bridge powers the toolbar *and* the keyboard.

- Threading: `TouchReader` runs on its own thread; the synthetic event must be constructed/sent on
  the GUI thread (queued `tap` signal → a GUI-thread slot that sends the event).
- Delivery primitive (resolve in the plan via a tiny spike): prefer the public
  `QCoreApplication::sendEvent(window, &mouseEvent)`; if that does not drive QtQuick hit-testing,
  fall back to `QWindowSystemInterface::handleMouseEvent` (private QPA header). Tap vs. swipe is
  already distinguishable from the existing decode (movement magnitude + dwell).

### 3.2 Toolbar: Qt Quick Controls, **Basic** style

A top bar (~96–120 px tall, big ≥88 px tap targets, black-on-white, flat) built from on-device
Controls: `Button` ×3 (Back, Forward, Reload) + a `TextField` (address). Basic style = least
chrome, highest contrast — ideal for e-ink. No custom drawing.

### 3.3 Navigation: WebKit's **native** API

All navigation reuses WPE WebKit directly — nothing hand-rolled:
`webkit_web_view_go_back / go_forward / reload / load_uri`, `webkit_web_view_can_go_back /
can_go_forward`, and the `notify::uri` + `load-changed` signals to keep the address field and
button enabled-state in sync. These calls are marshalled onto the engine's worker `GMainContext`
(same pattern as the existing `pageBy()`).

### 3.4 URL entry: **Qt Virtual Keyboard** (bundled), with a fallback

The user chose the standard Qt on-screen keyboard (no physical Type Folio). It is not on the
device, so we cross-build the `qtvirtualkeyboard` module against the SDK's Qt 6.8.2 (rmweb itself
links the device's system Qt, which is newer; same-major plugins built with an older minor load
fine) and bundle
its QML module + the `qtvirtualkeyboardplugin` input-context plugin; the launcher sets
`QT_IM_MODULE=qtvirtualkeyboard` and the QML import path. Tapping the address field focuses it and
the keyboard auto-appears (Qt VKB integrates with `TextField` via the input-method framework); its
keys are tapped through the §3.1 bridge.

**De-risk:** a short **spike** confirms VKB renders, accepts taps via the bridge, and commits text
to the focused field on the epaper QPA. **Fallback if intractable:** a minimal QML keypad (a
`Grid` of `Button`s appending characters to the same `TextField`) — same target, same bridge, no
input-method dependency. The fallback keeps Step 3 shippable regardless of VKB integration.

## 4. Architecture & data flow

```
finger evdev (event3, grabbed)
        │  TouchReader (own thread): classify
        ├── swipe → engine.pageBy(±page)           → WPE repaints → frame → WpeView → present
        └── tap(x,y) → [GUI thread] QMouseEvent → QQuickWindow → Qt Quick hit-test
                                   ├── toolbar Button  → engine.goBack/goForward/reload
                                   ├── address TextField → focus → Qt Virtual Keyboard
                                   └── VKB key          → TextField text → (Go) engine.loadUrl
engine signals: urlChanged / canGoBack / canGoForward → toolbar state (address text, button enabled)
QtQuick scene (toolbar + WpeView [+ VKB]) → QQuickWindow::afterRendering → EpaperRefresh::present() → e-ink
```

The whole chrome is part of the **same QtQuick scene** as `WpeView`, so it already reaches the
panel through the existing `EpaperRefresh` present path — no second display path.

## 5. Components & files

- **`engine/wpeqt/main.cpp`** (extend in place; it is the production app):
  - `TouchReader`: add tap classification + `void tap(int x, int y)` signal (keep `swipe`).
  - `WpeEngine`: add `goBack()`, `goForward()`, `reload()`, `loadUrl(QString)` (each marshalled to
    `m_ctx` like `pageBy`), and signals `urlChanged(QString)`, `canGoBack(bool)`,
    `canGoForward(bool)` driven from `notify::uri` / `load-changed` / `can_go_*`.
  - QML (`kQml`): toolbar (Controls, Basic) anchored top + `WpeView` filling the rest; bind buttons
    to engine slots and `enabled` to canGoBack/canGoForward; address `TextField` two-way with
    `urlChanged` / a Go action calling `loadUrl`.
  - `main()`: GUI-thread slot that converts `tap(x,y)` → `QMouseEvent` to the window; wire engine
    nav signals to the QML toolbar.
- **`engine/qtvirtualkeyboard.incontainer.sh`** (new): cross-build `qtvirtualkeyboard` 6.8.2 in the
  SDK container; stage the QML module (`…/qml/QtQuick/VirtualKeyboard`) + `qtvirtualkeyboardplugin`.
- **`scripts/bundle.sh`** (extend): ship the VKB QML module + input-context plugin under
  `/home/root/rmweb`.
- **`scripts/run-wpeqt-on-device.sh`** (extend): export `QT_IM_MODULE=qtvirtualkeyboard` and the
  `QML2_IMPORT_PATH` for the bundled VKB (show mode only).
- **No new module split**: the shell lives in the existing `wpeqt` app. (A later refactor can hoist
  the QML into its own file once it grows; not now — YAGNI.)

## 6. e-ink presentation notes

- `EpaperRefresh::present()` already fires from `afterRendering` on every QtQuick render, rate-
  limited to ~150 ms. Chrome changes (button highlight, TextField text, keyboard) cause a QtQuick
  render → a present, so chrome shows without extra plumbing. The web-frame **sig-dedup** lives in
  `WpeEngine::onBuffer` (drops duplicate *web* frames before they enter the scene); it does **not**
  gate `present()`, so it will not suppress chrome updates — verify this holds.
- Keep grayscale (`RMWEB_FULL_EVERY=0`) for reading; the keyboard/toolbar are grayscale UI. The
  ~150 ms present cadence makes typing feel like e-ink (each keypress lands within a frame) —
  acceptable; revisit in the deferred refresh-polish pass if needed.

## 7. Build sequence (each step: implement → verify on device → review → simplify)

- **Step 1 — HTTPS / real sites. ✅ DONE (2026-06-26).** example.com rendered over TLS. (Optional
  follow-up: capture a PNG of a text-rich article as a visual artifact.)
- **Step 2 — Toolbar + navigation + touch→mouse bridge.** The core "browser feel" and the bridge
  de-risk. Device test: tap each button (visible highlight + action), and exercise back/forward by
  seeding two `loadUrl`s (address entry arrives in Step 3, so Step 2's test drives nav from code or
  a temporary debug shortcut).
- **Step 3 — URL entry + Qt Virtual Keyboard.** Spike VKB integration first (fallback = QML keypad).
  Device test: tap address field → keyboard appears → type a URL → Go → page loads.

## 8. Risks & mitigations

1. **Synthetic mouse → QtQuick delivery** may need the private QPA primitive. *Mitigation:* tiny
   spike in Step 2; public `sendEvent` first, `QWindowSystemInterface` fallback.
2. **Qt VKB on the epaper QPA + IM framework** is the least-trodden path. *Mitigation:* Step 3 spike
   + QML-keypad fallback that needs no input-method integration.
3. **Cross-building `qtvirtualkeyboard` against the SDK Qt 6.8.2** (module may want Qt build
   artifacts). *Mitigation:* build the version matching the SDK Qt; if the SDK lacks pieces, the
   keypad fallback unblocks Step 3.
4. **Chrome present cadence / flicker.** *Mitigation:* reuse the existing present path; verify on
   device; defer fine-tuning to the planned refresh-polish pass.

## 9. Out of scope (MVP reading shell)

Tabs, bookmarks/history UI, link tap-to-navigate (web-area taps forwarding clicks to WebKit),
find-in-page, downloads, pinch-zoom, reading-mode reflow. Navigation is via toolbar + address bar
only. These belong to scope B (full browser) or the deferred polish pass.
