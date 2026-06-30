# Reading-Shell MVP — Phase A: Quality Reading Core — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status (2026-06-29):** Pure-logic layer ✅ DONE — `tapzone` + `refreshpolicy` (Phase A) and
`hintlabels` (Phase D, done early); runner `scripts/run-tests.sh`; all 5 host tests green
(`gesture, url, tapzone, refreshpolicy, hintlabels`). A4–A8 are now **grounded against the real
`engine/wpeqt/main.cpp`** (see "Grounding" below) and queued for the device+SDK session.

**Goal:** Turn the working WPE-on-e-ink engine into a *polished, fast* reading core: reader-first
fullscreen chrome that summons on a tap, edge-zone page-turn navigation, and an adaptive e-ink
refresh state machine that makes page turns land in ~120–250 ms (not the current fixed 2 s).

**Architecture:** Keep the 5-module split. The web engine is wrapped behind ONE QML façade
(Angelfish naming) all chrome binds to. New behavior is added as small pure helpers (host-tested)
plus QML chrome lifted from Angelfish/Liri (Controls 2, no Kirigami/Material).

**Tech Stack:** WPE WebKit 2.48 (CPU/Skia, JSC interpreter), Qt6 Qt Quick Controls 2 (epaper QPA),
Mesa llvmpipe, SQLite3. C/C++ cross-built via the ferrari SDK; pure-logic tests built on host with
clang++ `-std=c++17`.

**Design spec:** `docs/superpowers/specs/2026-06-29-rmweb-reading-shell-mvp-design.md`.
**Borrowed-pattern citations:** `docs/research/browser-ui-survey.md`.

## Global Constraints

- No GPU/EGL/GLES (software GL: Mesa llvmpipe, surfaceless EGL; paint = Skia CPU).
- Install ONLY under `/home/root/rmweb`; bundle missing libs, set rpath.
- Cross-compile only via the ferrari SDK (scarthgap, glibc 2.39, aarch64, `-mcpu=cortex-a53`).
- Display = Qt6 epaper QPA, QtQuick only, `QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper`, Window
  sized to `Screen.width/height`, xochitl stopped on run / restored on exit.
- **Present serialization mandatory:** never let two epaper presents overlap (the vendor present
  deadlocks on the framebuffer mutex — see the `eink-async-frame-display-bug` memory).
- JSC interpreter by default (`JSC_useJIT=0`); do not depend on the JIT.
- A process segfault reboots the device (~100 s). Keep `-rdynamic` + the SIGSEGV backtrace handler;
  logs under `/home/root`.
- **Quality gate (`quality-over-quantity-constrained-device` memory):** a feature ships only when it
  is fast + calm (no jank, no avoidable ghosting, instant chrome swaps, no animations). Prefer
  cutting scope to shipping something laggy.
- Respond to the user in Russian. Per task: implement → verify on device → code-review subagent →
  simplify subagent. `.env`/`build/` are gitignored and MUST NEVER be committed (guard every commit:
  `if git check-ignore -q .env; then commit; else ABORT`). Trailer: `Co-Authored-By: Claude Opus 4.8 …`.

---

## File Structure

**Created in Phase A:**
- `engine/wpeqt/tapzone.h` — pure tap-location → reading-action classifier (no Qt/device).
- `engine/wpeqt/refreshpolicy.h` — pure e-ink waveform policy (Fast/Full + every-N + ghost-clear).
- `tests/tapzone_test.cpp`, `tests/refreshpolicy_test.cpp` — host unit tests.
- `scripts/run-tests.sh` — build+run ALL `tests/*_test.cpp` on host (clang++).

**Modified in Phase A (device-verified tasks A4–A8):**
- `engine/wpeqt/main.cpp` — façade reshape, signal wiring, crash recovery, pagination, refresh
  controller, tap-zone wiring (consumes `tapzone.h`/`refreshpolicy.h`).
- `engine/wpeqt/*.qml` (or a new `shell/` QML set) — reader-first chrome + summon bars + e-ink theme.
- `scripts/run-wpeqt-on-device.sh` — pass any new env knobs.

**Phase ordering (quality-core-first; later phases = their own plans):**
- **Phase A (this plan):** A1–A3 device-free pure logic + runner (do now); A4–A8 engine/QML core
  (device-verified, execute when tablet+SDK available).
- **Phase B:** Reader mode (Readability.js) — separate plan.
- **Phase C:** Reading-device spine (SQLite persistence, smart bar autocomplete, start page,
  settings) — separate plan.
- **Phase D (optional tail, cuttable):** lite link-hinting (`hintlabels.h` + overlay) — separate plan.

---

## Task A1: Tap-zone classifier (`tapzone.h`) — DEVICE-FREE

**Files:**
- Create: `engine/wpeqt/tapzone.h`
- Test: `tests/tapzone_test.cpp`

**Interfaces:**
- Produces: `enum class rmweb::TapAction { Next, Prev, SummonChrome, Content }`;
  `struct rmweb::TapZones { double topStripFrac=0.08; double edgeFrac=0.22; }`;
  `rmweb::TapAction classifyTap(int x, int y, int w, int h, const TapZones& z = {})`.
- Consumed later by A8 (TouchReader → shell action routing).

**Model (design spec §3):** top strip → summon chrome; left/right edges → prev/next page;
center band → pass through to content (link taps). Top strip wins at the corners.

- [ ] **Step 1: Write the failing test** — `tests/tapzone_test.cpp`

```cpp
// Host unit test for the pure tap-zone classifier (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/tapzone_test tests/tapzone_test.cpp && ./build/tapzone_test
#include "../engine/wpeqt/tapzone.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    const int W = 1620, H = 2160;                 // rMPP panel
    // edgeFrac 0.22 -> left x<=356.4, right x>=1263.6 ; topStrip y<=172.8
    assert(classifyTap(W/2, H/2, W, H) == TapAction::Content);       // center -> link/content
    assert(classifyTap(10,   H/2, W, H) == TapAction::Prev);         // left edge
    assert(classifyTap(W-10, H/2, W, H) == TapAction::Next);         // right edge
    assert(classifyTap(W/2,  5,   W, H) == TapAction::SummonChrome); // top strip
    assert(classifyTap(5,    5,   W, H) == TapAction::SummonChrome); // top-left: top strip wins
    assert(classifyTap(10,   H-10,W, H) == TapAction::Prev);         // bottom-left = prev (no top)
    assert(classifyTap(356,  H/2, W, H) == TapAction::Prev);         // boundary <= edge -> prev
    assert(classifyTap(357,  H/2, W, H) == TapAction::Content);      // just inside center
    assert(classifyTap(10,   10,  0, 0) == TapAction::Content);      // zero-size guard
    printf("tapzone tests OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it, verify it FAILS to compile** (`tapzone.h` missing)

Run: `clang++ -std=c++17 -o build/tapzone_test tests/tapzone_test.cpp`
Expected: FAIL — `'../engine/wpeqt/tapzone.h' file not found`.

- [ ] **Step 3: Write the header** — `engine/wpeqt/tapzone.h`

```cpp
// Pure tap-location classifier for the reading shell — no Qt, no device deps, so it is unit-tested
// off-device (tests/tapzone_test.cpp). The shell maps the result: Next/Prev -> page turn,
// SummonChrome -> toggle bars, Content -> inject a mouse click (follow a link) via the touch bridge.
#pragma once
namespace rmweb {

enum class TapAction { Next, Prev, SummonChrome, Content };

// Edge-zone layout (fractions of panel w/h). Edges turn pages; the center stays tappable for links
// (a browser need a pure e-reader doesn't); the top strip summons chrome and wins at the corners.
struct TapZones {
    double topStripFrac = 0.08;  // top 8% summons chrome
    double edgeFrac     = 0.22;  // left/right 22% each = page turn
};

inline TapAction classifyTap(int x, int y, int w, int h, const TapZones& z = {}) {
    if (w <= 0 || h <= 0) return TapAction::Content;             // guard degenerate size
    if (y <= z.topStripFrac * h)        return TapAction::SummonChrome;
    if (x <= z.edgeFrac * w)            return TapAction::Prev;
    if (x >= (1.0 - z.edgeFrac) * w)    return TapAction::Next;
    return TapAction::Content;
}

} // namespace rmweb
```

- [ ] **Step 4: Run the test, verify it PASSES**

Run: `clang++ -std=c++17 -o build/tapzone_test tests/tapzone_test.cpp && ./build/tapzone_test`
Expected: `tapzone tests OK`.

- [ ] **Step 5: Commit** (with the `.env` guard)

```bash
if git check-ignore -q .env; then git add engine/wpeqt/tapzone.h tests/tapzone_test.cpp && \
git commit -q -m "feat(input): pure tap-zone classifier + host test" \
  -m "Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"; else echo ABORT; fi
```

---

## Task A2: E-ink refresh policy (`refreshpolicy.h`) — DEVICE-FREE

**Files:**
- Create: `engine/wpeqt/refreshpolicy.h`
- Test: `tests/refreshpolicy_test.cpp`

**Interfaces:**
- Produces: `enum class rmweb::Waveform { Fast, Full }`;
  `enum class rmweb::PresentKind { Navigation, PageTurn, Motion, Idle, Manual }`;
  `struct rmweb::RefreshPolicy { int fullEveryN=12; bool grayscaleMode=false; int turnsSinceFull=0;
   Waveform decide(PresentKind, bool hasColorContent); }`.
- Consumed later by A6 (the refresh controller around the epaper present).

**Model (design spec §4):** Motion → Fast (in-flight). Navigation/Manual → Full + reset. PageTurn →
Full when `every-N` hit or color content present (unless grayscaleMode), else Fast. Idle → Full only
if fast frames have accumulated (ghost-clear), else Fast.

- [ ] **Step 1: Write the failing test** — `tests/refreshpolicy_test.cpp`

```cpp
// Host unit test for the pure e-ink refresh policy (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/refreshpolicy_test tests/refreshpolicy_test.cpp && ./build/refreshpolicy_test
#include "../engine/wpeqt/refreshpolicy.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    // in-flight motion is always Fast and never resets the counter
    { RefreshPolicy p; assert(p.decide(PresentKind::Motion, false) == Waveform::Fast);
      assert(p.turnsSinceFull == 0); }
    // navigation = clean Full, counter reset
    { RefreshPolicy p; p.turnsSinceFull = 5;
      assert(p.decide(PresentKind::Navigation, false) == Waveform::Full);
      assert(p.turnsSinceFull == 0); }
    // manual full refresh = Full + reset
    { RefreshPolicy p; p.turnsSinceFull = 2;
      assert(p.decide(PresentKind::Manual, false) == Waveform::Full); assert(p.turnsSinceFull == 0); }
    // every-N: full flash on the Nth grayscale page turn, then back to fast
    { RefreshPolicy p; p.fullEveryN = 3;
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast);  // 1
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast);  // 2
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Full);  // 3 -> flush
      assert(p.turnsSinceFull == 0);
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast); } // 1 again
    // fullEveryN=0 disables the count path (only color/nav/manual force Full)
    { RefreshPolicy p; p.fullEveryN = 0;
      for (int i = 0; i < 50; ++i) assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast); }
    // color content forces Full each turn (ghosts harder) unless grayscale mode is on
    { RefreshPolicy p; p.fullEveryN = 0;
      assert(p.decide(PresentKind::PageTurn, true) == Waveform::Full); assert(p.turnsSinceFull == 0); }
    { RefreshPolicy p; p.fullEveryN = 0; p.grayscaleMode = true;
      assert(p.decide(PresentKind::PageTurn, true) == Waveform::Fast); } // color suppressed
    // idle ghost-clear: Full only if fast frames accumulated, else a no-op Fast
    { RefreshPolicy p; assert(p.decide(PresentKind::Idle, false) == Waveform::Fast); }
    { RefreshPolicy p; p.fullEveryN = 0; p.decide(PresentKind::PageTurn, false); // turnsSinceFull=1
      assert(p.decide(PresentKind::Idle, false) == Waveform::Full); assert(p.turnsSinceFull == 0); }
    printf("refreshpolicy tests OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it, verify it FAILS to compile** (`refreshpolicy.h` missing)

Run: `clang++ -std=c++17 -o build/refreshpolicy_test tests/refreshpolicy_test.cpp`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the header** — `engine/wpeqt/refreshpolicy.h`

```cpp
// Pure e-ink waveform policy for the present controller — no Qt, no device deps, so it is unit-tested
// off-device (tests/refreshpolicy_test.cpp). The controller calls decide() once per present and uses
// the result to pick a fast (grayscale, ~150 ms) vs full (GC16 colour flash, ~1-1.5 s) refresh.
#pragma once
namespace rmweb {

enum class Waveform { Fast, Full };
// What triggered this present. Motion = a frame mid swipe/scroll; PageTurn = a settled page after a
// turn; Navigation = new URL / reader toggle; Idle = the ~1-2 s idle ghost-clear; Manual = user asked.
enum class PresentKind { Navigation, PageTurn, Motion, Idle, Manual };

struct RefreshPolicy {
    int  fullEveryN     = 12;     // full flash every N page turns to clear ghosting (0 = never)
    bool grayscaleMode  = false;  // user "grayscale reading mode": suppress colour-driven full refresh
    int  turnsSinceFull = 0;      // fast page turns accumulated since the last full refresh

    Waveform decide(PresentKind kind, bool hasColorContent) {
        switch (kind) {
            case PresentKind::Motion:
                return Waveform::Fast;                      // in-flight: always fast, no counter change
            case PresentKind::Navigation:
            case PresentKind::Manual:
                turnsSinceFull = 0; return Waveform::Full;  // new page / explicit: clean full
            case PresentKind::Idle:
                if (turnsSinceFull == 0) return Waveform::Fast;   // nothing to ghost-clear
                turnsSinceFull = 0; return Waveform::Full;
            case PresentKind::PageTurn: {
                ++turnsSinceFull;
                const bool everyN = fullEveryN > 0 && turnsSinceFull >= fullEveryN;
                const bool color  = hasColorContent && !grayscaleMode;
                if (everyN || color) { turnsSinceFull = 0; return Waveform::Full; }
                return Waveform::Fast;
            }
        }
        return Waveform::Fast;
    }
};

} // namespace rmweb
```

- [ ] **Step 4: Run the test, verify it PASSES**

Run: `clang++ -std=c++17 -o build/refreshpolicy_test tests/refreshpolicy_test.cpp && ./build/refreshpolicy_test`
Expected: `refreshpolicy tests OK`.

- [ ] **Step 5: Commit** (with the `.env` guard, as in A1).

---

## Task A3: Host test runner (`scripts/run-tests.sh`) — DEVICE-FREE

**Files:** Create `scripts/run-tests.sh`.

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
set -euo pipefail
# Build + run ALL pure-logic host unit tests (no device, no SDK). clang++ C++17.
cd "$(dirname "$0")/.."
mkdir -p build
fail=0
for t in tests/*_test.cpp; do
  name="$(basename "$t" .cpp)"
  if clang++ -std=c++17 -Wall -Wextra -o "build/$name" "$t"; then
    "./build/$name" || { echo "FAIL (runtime): $name"; fail=1; }
  else
    echo "FAIL (compile): $name"; fail=1
  fi
done
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
```

- [ ] **Step 2: Make executable + run**

Run: `chmod +x scripts/run-tests.sh && ./scripts/run-tests.sh`
Expected: all of `gesture`, `url`, `tapzone`, `refreshpolicy` print `… tests OK`, then `ALL HOST TESTS OK`.

- [ ] **Step 3: Commit** (with the `.env` guard).

---

## Grounding — current engine (`engine/wpeqt/main.cpp` @ 3b35435) → Phase A changes

**Current state:**
- `WpeEngine` (worker): signals `frameReady, urlChanged, canGoBack, canGoForward`; slots `start, stop,
  pageBy(double), loadUrl, goBack, goForward, reload`. Wired: `load-changed`→`onLoadChanged` (emits
  canGoBack/Forward on COMMITTED/FINISHED), `notify::uri`→`onUri` (urlChanged), `buffer-rendered`→
  `onBuffer` (frameReady; drops dup frames by FNV sig). `pageBy` = `scrollBy + hidden-marker mutation
  → one repaint` (the verified pagination trick).
- `ShellBridge` (GUI proxy, exposed as `engine`): slots `goBack/goForward/reload/loadUrl`; signals
  `urlChanged/canGoBack/canGoForward`; QML binds via `Connections`.
- `WpeView`: present coalescer — `setImage` schedules one `update()` per `kPresentGapMs = 2000` (QTimer).
- `TouchReader`: emits `swipe(±1)`, `tap(x,y)`. `main()` maps swipe→`engine.pageBy(±kPageStepPx)`,
  tap→`sendClick` (UNCONDITIONAL touch→mouse bridge).
- `EpaperRefresh`: `dlopen` libqsgepaper `EPFramebuffer::instance/swapBuffers`. **Manual present GATED
  OFF** (`RMWEB_MANUAL_PRESENT`) — `swapBuffers` from `afterRendering` re-enters EPRenderLoop's
  framebuffer mutex → self-deadlock (proven). EPRenderLoop drives the panel when `update()` dirties it.
- `kQml`: **persistent top `ToolBar` + `WpeView`** (current layout = "persistent top", NOT reader-first).

**A4 façade:** keep `loadUrl/goBack/goForward/reload`; add engine `pageNext()/pagePrev()` (wrap
`pageBy(±kPageStepPx)`, move the mapping out of `main()`); stub Phase B/D `readerToggle, setReaderStyle,
findText…, setJsEnabled, hintStart, hintFollow`. Convert `ShellBridge` to a **Q_PROPERTY contract**
(`url, title, loading, loadProgress, canGoBack, canGoForward, readerable, readerMode, tlsOk, …`) with
NOTIFY so borrowed chrome binds `engine.canGoBack` directly; relay engine signals → property updates.

**A5 signals+crash:** have `load-changed`, `notify::uri`. ADD `g_signal_connect`:
`notify::estimated-load-progress`→loadProgress; `notify::title`→title; loading bool from `load-changed`
STARTED/FINISHED; `load-failed-with-tls-errors`+`webkit_web_view_get_tls_info()`→tls;
`web-process-terminated`→`processCrashed`→error page + auto-reload once (per-URL attempt guard).

**A6 present serializer (perf bet):** replace `WpeView`'s fixed `kPresentGapMs=2000` with
**`QQuickWindow::frameSwapped`-gated** release (frameSwapped fires after the scene swap = after
`EPFramebuffer::swapBuffers` returns) PLUS a waveform-aware minimum dwell from `refreshpolicy.decide()`
(Fast ~150 ms, Full ~1–1.5 s) — `swapBuffers` can return before the physical refresh completes (that
overlap was the deadlock). For a Full (colour/ghost-clear) frame, set `EPFramebuffer::setForceFull(true)`
(a flag setter — should not re-enter the mutex; verify) around the `update()`. **Never** resurrect the
`afterRendering`→`swapBuffers` manual-present path. Keep the 2000 ms proxy behind a fallback toggle.
Keep the `[gui] tick` heartbeat + `[present]`/`[t] frame` logs while tuning this (they diagnosed the
original deadlock).

**A7 reader-first chrome:** rewrite `kQml` to `WpeView` `anchors.fill` + top/bottom overlay bars whose
`visible` binds a `chromeVisible` prop; **instant** show/hide (no animation). Reuse the existing
Button/TextField/InputPanel; bind to A4 properties.

**A8 pagination+tap-zones:** route `TouchReader::tap` through `classifyTap(x,y,1620,2160)`:
`SummonChrome`→toggle `chromeVisible`; `Next`/`Prev`→`engine.pageNext/Prev`; `Content`→`sendClick`
(follow link). Keep `swipe`→page turn. When chrome is visible, taps within a bar rect still `sendClick`
so buttons work.

---

## Task A0: Land code-review fixes first (device build+verify) — ✅ DONE 2026-06-30

From the 2026-06-29 code review; both need the device (deferred from the host session):

- **C7 (critical) — `onFilterSaved` dangling `this`.** `WpeEngine`: add `GCancellable *m_cancel =
  g_cancellable_new();`; pass it to `webkit_user_content_filter_store_save(store,"rmweb-block",src,
  m_cancel,&onFilterSaved,this)`; in `stop()` call `g_cancellable_cancel(m_cancel)` before the loop-quit;
  early-return in `onFilterSaved` if `g_cancellable_is_cancelled(...)`; unref `m_cancel` at end of
  `start()`. Verify: blocking still activates on a normal load; a kill mid-load does not crash/reboot.
- **C17 (high) — touch coords 1 px out of bounds.** `TouchReader::run` ABS_MT_POSITION mapping: clamp
  `x = std::min(p.value*kPanelW/kTouchRawW, kPanelW-1)` and `y = std::min(..., kPanelH-1)`
  (`#include <algorithm>`). Verify: an extreme-edge tap still hits the intended control / classifies right.

Land as one small commit, build via `scripts/build-wpeqt.sh`, smoke on device, THEN start A4.

---

## Task A4: Engine façade reshape — ✅ DONE 2026-06-30 (nav-state verified on device)

**Files:** Modify `engine/wpeqt/main.cpp` (`WpeEngine`, `ShellBridge`).

**Interfaces — Produces (the contract all chrome binds to, design spec §1):**
- Props: `url, title, icon, loading, loadProgress, canGoBack, canGoForward, requestedUrl,
  readerMode, readerable, findCount, findIndex, jsEnabled, tlsOk, tlsHost`.
- Methods: `loadUrl, goBack, goForward, reload, stop, pageNext, pagePrev, readerToggle,
  setReaderStyle, findText, findNext, findPrev, findClear, setJsEnabled, hintStart, hintFollow`.
- Signals: `urlChanged, titleChanged, loadProgressChanged, navStateChanged, readerableChanged,
  readerModeChanged, tlsStateChanged, processCrashed, findResultChanged, hintsReady`.

**Approach:** rename/extend the existing `ShellBridge` Q_PROPERTYs / Q_INVOKABLEs to the names above
(Angelfish-compatible). Keep the established cross-thread rule: QML↔engine via `ShellBridge` on the
GUI thread; engine work marshalled with `g_timeout_source_new` + `g_source_attach(m_ctx)`. Read the
current `main.cpp` first and map existing members onto the new names (do not rewrite the engine).

**Steps (expand into bite-sized TDD at execution):** read `main.cpp` → list current bridge members →
rename to the contract → build via `scripts/build-wpeqt.sh` → `scripts/run-wpeqt-on-device.sh save` →
confirm a page still loads + the bridge props update. **Verify:** address bar shows `url`, title
updates, back/fwd enable correctly.

---

## Task A5: Epiphany signal wiring + WebProcess-crash recovery — ✅ DONE 2026-06-30 (verified on device)

**Files:** Modify `engine/wpeqt/main.cpp`.

**Approach (design spec §1, §9):** connect `load-changed`, `notify::estimated-load-progress`,
`notify::title`/`uri`, `is-loading`, `can_go_back/forward`, `load-failed-with-tls-errors` +
`get_tls_info()`, and `web-process-terminated`. On `web-process-terminated` emit `processCrashed` →
shell shows an error page and auto-reloads **once** (per-URL attempt counter + backoff to avoid a
reload loop). **Verify on device:** progress + title + nav-state track real loads; killing the
WebProcess (`kill` its PID) shows the error page and a single auto-reload, no device reboot.

---

## Task A6: Present serializer — ✅ DONE 2026-06-30 (~130ms turns, frameSwapped-gated); refreshpolicy waveform/ghost-clear deferred

**Files:** Modify `engine/wpeqt/main.cpp` (`WpeView`/present path); consume `refreshpolicy.h`.

**Approach (design spec §4; `eink-async-frame-display-bug` memory):** keep "at most one present in
flight, coalesce to the latest frame", but **replace the fixed `kPresentGapMs=2000` with
completion-gated serialization** — gate the next present on actual panel-refresh completion (epaper
completion signal if exposed, else a waveform-aware timeout: Fast ≈ 150 ms, Full ≈ 1–1.5 s). Pick the
waveform via `RefreshPolicy::decide(kind, hasColor)` and drive `EPFrameBuffer::setForceFull` for Full.
This both keeps the deadlock impossible AND unlocks fast turns. **HIGHEST-RISK TASK — verify
carefully:** a long auto-page run must never overlap presents (no freeze), and turns must land in
~120–250 ms (measure on device). Gate behind a toggle so we can fall back to the 2 s proxy if needed.

---

## Task A7: Reading-first chrome (QML) — DEVICE-VERIFIED

**Files:** `engine/wpeqt/*.qml` (or new `shell/`): main reader-first window, `Navigation` bar,
bottom progress bar, `Menu`, `Theme`/`*Themed` e-ink skin.

**Approach (design spec §2; survey §4):** fullscreen WebView item; tap the top strip (or two-finger
tap) toggles top bar (back/fwd/reload-stop/address-affordance/reader/menu) + bottom bar (progress +
page %). **Instant swap, no slide animation.** Lift `Navigation.qml` (Controls 2 `RowLayout` of
`ToolButton`) from Angelfish; reskin via Liri's `*Themed` pattern (black-on-white, no gradients/
shadows/large black fills). **Verify on device:** bars summon/dismiss instantly, no ghosting, chrome
reads live façade props.

---

## Task A8: Pagination + tap-zone wiring — DEVICE-VERIFIED

**Files:** `engine/wpeqt/main.cpp` (`pageNext`/`pagePrev`), `input` TouchReader (consume `tapzone.h`).

**Approach (design spec §3; `wpe-rendering-protocol.md`):** `pageNext/pagePrev` = `scrollBy` one
viewport height (minus small overlap), clamped, **followed by a tiny DOM mutation to force an
immediate repaint** (bare `scrollBy` emits no buffer). Route taps through `classifyTap` (A1):
Next/Prev → page turn, SummonChrome → toggle bars, Content → inject a mouse click via the existing
touch→mouse bridge. Keep the existing swipe path (`gesture.h`: swipe-up → next, swipe-down → prev).
**Verify on device:** edge taps + swipes turn pages cleanly on boundaries; center taps follow links;
top strip summons chrome; turns are fast (A6).

---

## Self-Review

- **Spec coverage (Phase A slice):** façade §1→A4; signals/crash §1,§9→A5; refresh §4→A2+A6;
  pagination/tap-zones §3→A1+A8; reading-first chrome §2→A7. Reader mode §5, persistence/smart bar/
  start page §6, link-hinting §7 are deferred to Phases B/C/D (own plans) — intentional, per
  quality-core-first. ✓
- **Placeholders:** none in A1–A3 (complete code). A4–A8 are device-verified task specs whose
  bite-sized TDD steps are expanded at execution against the real `main.cpp` (can't compile/verify
  off-device) — flagged explicitly, not hidden TODOs. ✓
- **Type consistency:** `TapAction`/`TapZones`/`classifyTap` and `Waveform`/`PresentKind`/
  `RefreshPolicy::decide` names identical across header, test, and consuming tasks (A6/A8). ✓
