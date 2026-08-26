# Reading Shell Implementation Plan

> **For agentic workers:** Execution model is **inline** (per the project's CLAUDE.md working
> agreement: implement → verify on device → code-review subagent → simplify subagent). The author
> holds the device/build context, so tasks are executed in-session rather than by zero-context
> subagents. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wrap the working WPE web view in a minimal reading-browser shell — a toolbar
(back/forward/reload + address), real URL navigation, and on-screen URL entry via Qt Virtual
Keyboard — all on the e-ink Paper Pro, reusing existing components.

**Architecture:** A **touch→mouse bridge** turns finger taps (already grabbed by `TouchReader`) into
synthetic `QMouseEvent`s delivered to the `QQuickWindow`, so Qt Quick Controls hit-test and handle
them natively. Toolbar = Qt Quick Controls (Basic). Navigation = WebKit's native API, marshalled to
the engine's worker `GMainContext`. The whole chrome is one QtQuick scene that already reaches the
panel via the existing `EpaperRefresh` present path. URL entry = bundled Qt Virtual Keyboard
(fallback: minimal QML keypad).

**Tech Stack:** C++17, Qt 6.8.2 (QtQuick + QtQuick.Controls.Basic + QtQuick.VirtualKeyboard), WPE
WebKit 2.48.5, GLib, the ferrari aarch64 Yocto SDK (cross-compile in the `rmweb-sdk` container),
host `clang++` for pure-logic unit tests.

## Global Constraints

- **Device verification is mandatory** after each task (CLAUDE.md): build via `scripts/build-wpeqt.sh`,
  deploy+run via `scripts/run-wpeqt-on-device.sh show <url>` with `xochitl` stopped/restored.
- **Crash-safety is paramount:** a segfault reboots the device (~100 s). Keep the unbounded-wait
  teardown; never `wpe_view_buffer_released()` on an embedded view; keep `JSC_useJIT=0`.
- **No GPU driver** (the SoC has a GPU on die, but the stock OS ships no driver — CPU-only in practice): run with `GALLIUM_DRIVER=llvmpipe` (already in the run script).
- **Install only under `/home/root/rmweb`**; bundle missing libs; reuse on-device Qt/Controls.
- **Touch is ours:** `TouchReader` `EVIOCGRAB`s event3 ("Elan touch input"); do not un-grab (the grab
  also silences the epaper QPA's crashing touch dispatch).
- **Reading present mode:** grayscale (`RMWEB_FULL_EVERY=0`), ~150 ms present rate-limit — keep.
- **Commit guard (security):** every commit runs `git check-ignore -q .env` first and aborts if `.env`
  is not ignored. Stage only the files the task names — never `.env` or `build/`.
- **Commit trailer:** `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## File Structure

- `engine/wpeqt/gesture.h` *(new)* — pure tap/swipe classifier (no Qt) → host-unit-testable.
- `engine/wpeqt/url.h` *(new)* — pure URL normalizer (no Qt) → host-unit-testable.
- `engine/wpeqt/main.cpp` *(modify)* — `TouchReader` tap signal; touch→mouse bridge; `WpeEngine`
  nav API + state signals; QML toolbar + VKB; context-property wiring.
- `tests/gesture_test.cpp`, `tests/url_test.cpp` *(new)* — host unit tests (compiled with `clang++`).
- `engine/qtvirtualkeyboard.incontainer.sh` *(new)* — cross-build Qt Virtual Keyboard 6.8.2.
- `scripts/bundle.sh` *(modify)* — ship the VKB QML module + input-context plugin.
- `scripts/run-wpeqt-on-device.sh` *(modify)* — export `QT_IM_MODULE` + QML/plugin paths (show mode).

---

# STEP 2 — Toolbar + navigation + touch→mouse bridge

### Task 1: Tap/swipe classifier (pure logic, TDD) + `tap` signal

**Files:**
- Create: `engine/wpeqt/gesture.h`
- Create: `tests/gesture_test.cpp`
- Modify: `engine/wpeqt/main.cpp` (`TouchReader`)

**Interfaces:**
- Produces: `rmweb::Gesture classifyGesture(int dx, int dy, int dwellMs, GestureParams={})` and
  `TouchReader::tap(int x, int y)` (Qt signal, panel px at lift).
- Consumes: existing `TouchReader` decode (sx/sy at down, x/y at lift).

- [ ] **Step 1: Write the failing host test** — `tests/gesture_test.cpp`:

```cpp
#include "../engine/wpeqt/gesture.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    assert(classifyGesture(0, -300, 200) == Gesture::SwipeUp);    // up = next page
    assert(classifyGesture(10, 300, 200) == Gesture::SwipeDown);  // down = prev page
    assert(classifyGesture(5, 5, 120)   == Gesture::Tap);         // small move, short dwell
    assert(classifyGesture(5, 5, 2000)  == Gesture::None);        // long press is NOT a tap
    assert(classifyGesture(300, 300, 200) == Gesture::None);      // diagonal -> nothing
    assert(classifyGesture(0, 100, 200)   == Gesture::None);      // short vertical -> nothing
    printf("gesture tests OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails** — `clang++ -std=c++17 -o build/gesture_test tests/gesture_test.cpp`
      Expected: FAIL to compile ("gesture.h: No such file").

- [ ] **Step 3: Write `engine/wpeqt/gesture.h`:**

```cpp
#pragma once
namespace rmweb {
enum class Gesture { None, SwipeUp, SwipeDown, Tap };
struct GestureParams {
    int swipeMinDy   = 240;  // vertical travel (px) to count as a page turn
    int swipeMaxDx   = 200;  // keep a swipe roughly vertical
    int tapMaxMove   = 40;   // max travel (px) to still be a tap
    int tapMaxDwellMs = 700; // max contact duration (ms) for a tap (longer = long-press = ignore)
};
// dx,dy = lift - down (panel px); dwellMs = contact duration. Pure: unit-tested off-device.
inline Gesture classifyGesture(int dx, int dy, int dwellMs, const GestureParams& p = {}) {
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    if (adx <= p.tapMaxMove && ady <= p.tapMaxMove && dwellMs <= p.tapMaxDwellMs)
        return Gesture::Tap;
    if (adx < p.swipeMaxDx && ady >= p.swipeMinDy)
        return dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown;
    return Gesture::None;
}
} // namespace rmweb
```

- [ ] **Step 4: Run the test to verify it passes** — same compile + `./build/gesture_test`
      Expected: `gesture tests OK`.

- [ ] **Step 5: Wire it into `TouchReader`** (`engine/wpeqt/main.cpp`): `#include "gesture.h"`; record
      `m_downUs = g_get_monotonic_time()` when `pendingDown` resolves at SYN; add `Q_SIGNALS: void tap(int x, int y);`
      replace `emitSwipe(dx,dy)` body to classify and dispatch:

```cpp
void emitGesture(int dx, int dy, int x, int y, gint64 downUs) {
    const int dwellMs = static_cast<int>((g_get_monotonic_time() - downUs) / 1000);
    switch (classifyGesture(dx, dy, dwellMs)) {
    case Gesture::Tap: {
        const gint64 now = g_get_monotonic_time();
        if (m_lastTapUs && now - m_lastTapUs < 250000) return;  // debounce double-taps
        m_lastTapUs = now;
        qInfo("[touch] tap @ %d,%d", x, y);
        Q_EMIT tap(x, y);
        return;
    }
    case Gesture::SwipeUp:
    case Gesture::SwipeDown: {
        const gint64 now = g_get_monotonic_time();
        if (m_lastSwipeUs && now - m_lastSwipeUs < 800000) return;  // <=1 turn / 0.8 s
        m_lastSwipeUs = now;
        const int dir = (dy < 0) ? +1 : -1;
        qInfo("[touch] swipe %s -> %s", dir > 0 ? "up" : "down", dir > 0 ? "next" : "prev");
        Q_EMIT swipe(dir);
        return;
    }
    case Gesture::None: return;
    }
}
```
      (Add `gint64 m_lastTapUs = 0;` and pass the latched down-time/lift-pos from the SYN handler.)

- [ ] **Step 6: Build for device** — `scripts/build-wpeqt.sh`; expect a clean binary (no warnings on the new code).

- [ ] **Step 7: Commit** (guarded):

```bash
git check-ignore -q .env && git add engine/wpeqt/gesture.h tests/gesture_test.cpp engine/wpeqt/main.cpp \
  && git commit -m "Phase 4: tap/swipe classifier (host-tested) + TouchReader tap signal"
```

### Task 2: Touch→mouse bridge (the keystone de-risk)

**Files:** Modify `engine/wpeqt/main.cpp` (`main()` display branch).

**Interfaces:**
- Consumes: `TouchReader::tap(int,int)`, the root `QQuickWindow*`.
- Produces: synthetic `QMouseEvent` press+release delivered to the window on the GUI thread.

- [ ] **Step 1: Add the bridge** — after the QML root/window is created, with `#include <QMouseEvent>`:

```cpp
QObject::connect(&touchReader, &TouchReader::tap, &app, [win](int x, int y) {
    const QPointF pt(x, y);
    QMouseEvent press(QEvent::MouseButtonPress, pt, pt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pt, pt, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(win, &press);
    QCoreApplication::sendEvent(win, &release);
}, Qt::QueuedConnection);   // run on the GUI thread (TouchReader emits from its own thread)
```
      where `win` is `qobject_cast<QQuickWindow*>(root)`.

- [ ] **Step 2: Add a TEMP debug button to `kQml`** to prove routing: a `Button` at a known spot with
      `onClicked: console.log("[qml] debug button clicked")` (removed in Task 4).

- [ ] **Step 3: Device verify** — `scripts/run-wpeqt-on-device.sh show https://example.com`; tap the debug
      button. Expected log: `[touch] tap @ x,y` followed by `[qml] debug button clicked`. **If the click does
      not route**, switch delivery to the QPA primitive (private header
      `#include <qpa/qwindowsysteminterface.h>`):
      `QWindowSystemInterface::handleMouseEvent(win, pt, pt, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress, Qt::NoModifier);`
      (+ a matching release). Re-test. Keep whichever routes; record the verdict in a code comment.

- [ ] **Step 4: Commit** — `git add engine/wpeqt/main.cpp && git commit -m "Phase 4: touch->mouse bridge (tap -> QtQuick Controls)"`.

### Task 3: `WpeEngine` navigation API + state signals

**Files:** Modify `engine/wpeqt/main.cpp` (`WpeEngine`).

**Interfaces:**
- Produces (public slots, also `Q_INVOKABLE` for QML): `loadUrl(const QString&)`, `goBack()`,
  `goForward()`, `reload()`. Signals: `urlChanged(QString)`, `canGoBack(bool)`, `canGoForward(bool)`.
- Consumes: WebKit `webkit_web_view_{load_uri,go_back,go_forward,reload,can_go_back,can_go_forward,get_uri}`,
  signals `notify::uri` and `load-changed`.

- [ ] **Step 1: Add a marshal helper** (replaces the ad-hoc `PageMsg`; `pageBy` can adopt it too):

```cpp
void marshalToCtx(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    g_main_context_invoke_full(m_ctx, G_PRIORITY_DEFAULT,
        [](gpointer d) -> gboolean { (*static_cast<std::function<void()>*>(d))(); return G_SOURCE_REMOVE; },
        f, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
}
```
      (`#include <functional>`; `#include "url.h"` lands in Task 5.)

- [ ] **Step 2: Add nav slots** (run on whatever thread calls; `g_main_context_invoke_full` is MT-safe):

```cpp
public Q_SLOTS:
    void loadUrl(const QString& u)  { auto s = u.toStdString(); marshalToCtx([this,s]{ if(m_view) webkit_web_view_load_uri(m_view, s.c_str()); }); }
    void goBack()    { marshalToCtx([this]{ if(m_view && webkit_web_view_can_go_back(m_view))    webkit_web_view_go_back(m_view); }); }
    void goForward() { marshalToCtx([this]{ if(m_view && webkit_web_view_can_go_forward(m_view)) webkit_web_view_go_forward(m_view); }); }
    void reload()    { marshalToCtx([this]{ if(m_view) webkit_web_view_reload(m_view); }); }
Q_SIGNALS:
    void urlChanged(const QString& url);
    void canGoBack(bool ok);
    void canGoForward(bool ok);
```

- [ ] **Step 3: Emit state** — in `start()` after creating `m_view`, connect:

```cpp
g_signal_connect(m_view, "notify::uri", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer d){
    auto* self = static_cast<WpeEngine*>(d);
    const char* u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(o));
    Q_EMIT self->urlChanged(QString::fromUtf8(u ? u : ""));
}), this);
```
      and extend `onLoadChanged` (any event, not just FINISHED) to emit
      `canGoBack(webkit_web_view_can_go_back(view))` and `canGoForward(...)`. Cross-thread Qt signals
      to the GUI use auto/queued delivery — safe.

- [ ] **Step 4: Device verify** — temporary in `start()` after load: seed history with a `QTimer`/JS-free
      `marshalToCtx` sequence (load A `https://example.com`, then after 3 s load B `https://example.org`,
      then after 3 s `goBack()`), with `RMWEB_AUTOPAGE_MS` unset. Run `show`. Expected log:
      `urlChanged` to example.org, then back to example.com; `canGoBack`/`canGoForward` toggling true.
      Remove the seed after verifying.

- [ ] **Step 5: Commit** — `git add engine/wpeqt/main.cpp && git commit -m "Phase 4: WpeEngine navigation API + url/canGo state signals"`.

### Task 4: QML toolbar (Controls Basic) + context-property wiring

**Files:** Modify `engine/wpeqt/main.cpp` (`kQml` + display-branch wiring).

**Interfaces:**
- Consumes: `engine` exposed as a QML context property; `WpeView` registered type.
- Produces: a toolbar whose buttons call `engine.goBack/goForward/reload`, an address `TextField`
  (read-only this task), and `Connections` that reflect engine state.

- [ ] **Step 1: Replace `kQml`** with an `ApplicationWindow` + toolbar (forces Basic style via the import):

```cpp
static const char *kQml = R"QML(
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts
import rmweb 1.0
ApplicationWindow {
    id: win
    width: Screen.width; height: Screen.height
    visible: true; color: "white"
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        ToolBar {
            Layout.fillWidth: true; implicitHeight: 104
            background: Rectangle { color: "white"; border.color: "black"; border.width: 2 }
            RowLayout {
                anchors.fill: parent; anchors.margins: 6; spacing: 8
                Button { id: back; text: "◀"; implicitWidth: 104; implicitHeight: 88; enabled: false; onClicked: engine.goBack() }
                Button { id: fwd;  text: "▶"; implicitWidth: 104; implicitHeight: 88; enabled: false; onClicked: engine.goForward() }
                Button { id: rel;  text: "↻"; implicitWidth: 104; implicitHeight: 88;                 onClicked: engine.reload() }
                TextField {
                    id: address; Layout.fillWidth: true; implicitHeight: 88; font.pixelSize: 34
                    readOnly: true; selectByMouse: false
                    onAccepted: engine.loadUrl(text)   // wired live in Step 3
                }
            }
        }
        WpeView { objectName: "view"; Layout.fillWidth: true; Layout.fillHeight: true }
    }
    Connections {
        target: engine
        function onUrlChanged(url)      { if (!address.activeFocus) address.text = url }
        function onCanGoBack(ok)        { back.enabled = ok }
        function onCanGoForward(ok)     { fwd.enabled = ok }
    }
}
)QML";
```

- [ ] **Step 2: Expose the engine to QML** — in the display branch, before `comp->create()`:
      `qmlEngine->rootContext()->setContextProperty("engine", &engine);`
      Keep `qmlRegisterType<WpeView>(...)`. The root is now an `ApplicationWindow` (a `QQuickWindow`) —
      the existing `qobject_cast<QQuickWindow*>(root)` for `EpaperRefresh` + the touch bridge still holds.

- [ ] **Step 3: Remove the Task-2 debug button** from the QML.

- [ ] **Step 4: Device verify** — `show https://example.com`. Expected: toolbar visible on e-ink with three
      buttons + address showing the URL; tapping `⟳` reloads (log shows reload + a NEW frame); after the
      Task-3-style seeded history, `◀`/`▶` enable and navigate. Confirm no reboot (uptime holds).

- [ ] **Step 5: Code-review + simplify (subagents)** — dispatch a `feature-dev:code-reviewer` over the
      Step-2 diff (touch bridge thread-safety, signal/slot affinity, QML wiring leaks, teardown still safe),
      then a `code-simplifier` over the same. Apply real findings.

- [ ] **Step 6: Commit** — `git add engine/wpeqt/main.cpp && git commit -m "Phase 4: reading-shell toolbar (Controls Basic) + WebKit nav wiring"`.

---

# STEP 3 — URL entry + Qt Virtual Keyboard

### Task 5: URL normalizer (pure logic, TDD)

**Files:** Create `engine/wpeqt/url.h`, `tests/url_test.cpp`; use it inside `WpeEngine::loadUrl`.

**Interfaces:** Produces `std::string rmweb::normalizeUrl(std::string)`.

- [ ] **Step 1: Failing host test** — `tests/url_test.cpp`:

```cpp
#include "../engine/wpeqt/url.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    assert(normalizeUrl("example.com")        == "https://example.com");
    assert(normalizeUrl("  example.com  ")    == "https://example.com");
    assert(normalizeUrl("http://x.org")       == "http://x.org");
    assert(normalizeUrl("https://y.org/path") == "https://y.org/path");
    assert(normalizeUrl("")                    == "");
    printf("url tests OK\n");
    return 0;
}
```

- [ ] **Step 2: Run, expect FAIL** — `clang++ -std=c++17 -o build/url_test tests/url_test.cpp` (no `url.h`).

- [ ] **Step 3: Write `engine/wpeqt/url.h`:**

```cpp
#pragma once
#include <string>
#include <algorithm>
#include <cctype>
namespace rmweb {
inline std::string normalizeUrl(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    if (s.empty()) return s;
    if (s.find("://") == std::string::npos) s = "https://" + s;
    return s;
}
} // namespace rmweb
```

- [ ] **Step 4: Run, expect PASS** — `./build/url_test` → `url tests OK`.

- [ ] **Step 5: Use it** — in `WpeEngine::loadUrl`, normalize first:
      `auto s = normalizeUrl(u.toStdString());` then load `s`.

- [ ] **Step 6: Commit** — `git add engine/wpeqt/url.h tests/url_test.cpp engine/wpeqt/main.cpp \
      && git commit -m "Phase 4: URL normalizer (host-tested), used by loadUrl"`.

### Task 6: Cross-build Qt Virtual Keyboard 6.8.2

**Files:** Create `engine/qtvirtualkeyboard.incontainer.sh`.

**Interfaces:** Produces, under `build/stage-vkb/`: the QML module `qml/QtQuick/VirtualKeyboard/**`
and the input-context plugin `plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so`, built
against the SDK's Qt 6.8.2 for aarch64.

- [ ] **Step 1: Write the build script** — fetch the `qtvirtualkeyboard` 6.8.2 source, configure with the
      SDK's `qt-cmake`/CMake against the sysroot Qt, build, install to a staging prefix. Skeleton:

```bash
#!/usr/bin/env bash
set -euo pipefail   # runs INSIDE the rmweb-sdk container (native aarch64 on Apple Silicon)
SRC=/build/qtvk/qtvirtualkeyboard-6.8.2
STAGE=/work/build/stage-vkb
[ -d "$SRC" ] || { mkdir -p "$(dirname "$SRC")"; \
  curl -L https://download.qt.io/archive/qt/6.8/6.8.2/submodules/qtvirtualkeyboard-everywhere-src-6.8.2.tar.xz \
    | tar -xJ -C "$(dirname "$SRC")"; mv "$(dirname "$SRC")"/qtvirtualkeyboard-* "$SRC"; }
cmake -S "$SRC" -B "$SRC/_b" -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$OECORE_NATIVE_SYSROOT/usr/lib/cmake/Qt6/qt.toolchain.cmake" \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release \
  -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
cmake --build "$SRC/_b"
DESTDIR="$STAGE" cmake --install "$SRC/_b"
echo "[vkb] staged:"; find "$STAGE" -name 'libqtvirtualkeyboardplugin.so' -o -name 'qmldir' | head
```
      (Exact toolchain-file path/flags verified against the SDK at build time — the SDK ships Qt dev, so
      `qt.toolchain.cmake` exists; adjust `-DCMAKE_PREFIX_PATH` if needed. Run via the project's container
      entrypoint, mirroring `engine/mesa-llvmpipe.incontainer.sh`.)

- [ ] **Step 2: Build it** — invoke through the existing container runner; confirm the two artifacts exist
      under `build/stage-vkb/`. **If the SDK lacks pieces to build VKB**, STOP here and switch Step 3 to the
      QML-keypad fallback (Task 8, fallback branch) — do not sink time; the fallback ships URL entry.

- [ ] **Step 3: Commit** — `git add engine/qtvirtualkeyboard.incontainer.sh \
      && git commit -m "Phase 4: cross-build Qt Virtual Keyboard 6.8.2"`.

### Task 7: Bundle the VKB + launcher env

**Files:** Modify `scripts/bundle.sh`, `scripts/run-wpeqt-on-device.sh`.

**Interfaces:** Consumes `build/stage-vkb/`. Produces `/home/root/rmweb/qml/QtQuick/VirtualKeyboard/**`
and `/home/root/rmweb/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so` on device; the
launcher exports the input-method module + import/plugin paths.

- [ ] **Step 1: Extend `bundle.sh`** — after the llvmpipe block (per-item copy with a loud WARN if missing,
      matching the existing style):

```bash
if [ -d build/stage-vkb/usr/qml/QtQuick/VirtualKeyboard ]; then
  mkdir -p "$B/qml/QtQuick" "$B/plugins/platforminputcontexts"
  cp -a build/stage-vkb/usr/qml/QtQuick/VirtualKeyboard "$B/qml/QtQuick/"
  cp -a build/stage-vkb/usr/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so "$B/plugins/platforminputcontexts/" \
    || echo "[bundle] WARN: VKB plugin missing"
else echo "[bundle] WARN: no build/stage-vkb — URL entry will fall back to the QML keypad"; fi
```

- [ ] **Step 2: Extend the launcher** (`show` branch of `run-wpeqt-on-device.sh`):
      `export QT_IM_MODULE=qtvirtualkeyboard`
      `export QML2_IMPORT_PATH="$R/qml"`
      `export QT_PLUGIN_PATH="$R/plugins:${QT_PLUGIN_PATH:-/usr/lib/plugins}"`
      (Keep `QT_QPA_PLATFORM=epaper`; the input-context plugin loads independently of the platform plugin.)

- [ ] **Step 3: Device verify** — `scripts/bundle.sh` then `show https://example.com`. Expected: no QML
      import error for `QtQuick.VirtualKeyboard` in the log; app runs as before.

- [ ] **Step 4: Commit** — `git add scripts/bundle.sh scripts/run-wpeqt-on-device.sh \
      && git commit -m "Phase 4: bundle Qt Virtual Keyboard + launcher IM env"`.

### Task 8: Editable address + on-screen keyboard (spike, with keypad fallback)

**Files:** Modify `engine/wpeqt/main.cpp` (`kQml`).

**Interfaces:** Consumes the bundled VKB + `engine.loadUrl`. Produces a working type-a-URL-and-go flow.

- [ ] **Step 1: Make the address field editable + add the InputPanel** to `kQml`:
      set `address.readOnly: false; selectByMouse: true; inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase`,
      and add the keyboard:

```cpp
// at the top of kQml: import QtQuick.VirtualKeyboard
// inside ApplicationWindow, as a sibling overlaying the bottom:
InputPanel {
    id: inputPanel
    z: 99
    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
    visible: active                 // shown only while a field has active focus
}
```
      `onAccepted: engine.loadUrl(text)` already calls the normalizer (Task 5).

- [ ] **Step 2: Device verify (the spike)** — `show https://example.com`. Tap the address field (touch→mouse
      bridge focuses it) → the VKB should appear → tap keys → characters enter the field → tap Enter/return
      key → the page loads. Expected log: `urlChanged` to the typed URL + a NEW frame. Confirm the keyboard
      and toolbar render on e-ink and no reboot.

- [ ] **Step 3 (only if Step 2 fails): Keypad fallback** — replace `InputPanel` with a minimal
      `Grid` of `Button`s (a–z, `.`, `/`, `:`, ⌫, Go) that mutate `address.text` and call `engine.loadUrl`
      on Go. No input-method dependency; works purely through the touch→mouse bridge. Re-verify on device.
      Record which path shipped in a code comment + update the spec's §3.4 outcome.

- [ ] **Step 4: Code-review + simplify (subagents)** over the Step-3 diff (focus handling, VKB lifetime,
      present cadence while typing, fallback branch correctness). Apply real findings.

- [ ] **Step 5: Commit** — `git add engine/wpeqt/main.cpp \
      && git commit -m "Phase 4: on-screen URL entry (Qt Virtual Keyboard | keypad fallback)"`.

---

## Done criteria

- Toolbar (◀ ▶ ⟳ + address) renders on e-ink; buttons navigate real sites; back/forward enable-state
  tracks history; reload works.
- Tapping the address field brings up an on-screen keyboard; typing a bare host (`example.com`) loads
  `https://example.com`.
- No device reboot across a full session; page-turn swipes still work alongside the chrome.
- `gesture_test` and `url_test` pass on the host.

## Deferred (not this plan)

Refresh flicker/ghosting polish (text crispness vs. ghosting, periodic ghost-clear, `RMWEB_FULL_EVERY`
default), link tap-to-navigate in the web area, tabs/bookmarks/history UI — all per the spec's §9 and
the user's "polish after we have a working browser" directive.
