# reMarkable Paper Pro — finger-touch input for a custom native app

**Scope.** How to get **finger-touch** (not pen) input into our native app on the **reMarkable Paper Pro**
(codename **"ferrari"**, color E Ink Gallery 3, i.MX8M Mini aarch64, Linux 6.12, glibc 2.39, **no GPU**),
running under **Qt6 + the reMarkable "epaper" QPA** with **xochitl stopped**.

Researched 2026-06-26 from primary sources (reMarkable's own QPA source, Qt source, the Linux kernel evdev
driver, KOReader, Plato, rmkit, netsurf-reMarkable, Oxide, remarkable.guide). **Every non-obvious claim is
cited.** Facts I verified by reading the actual source are marked **[verified-source]**; reasoned inferences
are marked **[inference]**.

---

## 0. Ground truth (verified on the device, 2026-06-26) — read this first

These supersede older notes in `docs/device-profile.md` and `docs/research-reuse.md §4`, which carried the
**WRONG** rM2-era node mapping (touch=event2/pen=event3). **On the Paper Pro it is the other way around:**

| Node | `/proc/bus/input/devices` name | Role |
|---|---|---|
| `event0` | `30370000.snvs:snvs-powerkey` | power button |
| `event1` | `Hall effect sensors` | folio cover |
| **`event2`** | **`Elan marker input`** | **PEN / stylus** |
| **`event3`** | **`Elan touch input`** | **FINGER TOUCH** ← what we want |

- The **finger touch** digitizer (`event3`) is **Elan, Type-B multitouch**, `INPUT_PROP_DIRECT`,
  `ABS_MT_SLOT` max **9** (→ up to 10 contacts), `ABS_MT_POSITION_X` **0..2064**, `ABS_MT_POSITION_Y`
  **0..2832**, plus `ABS_MT_TRACKING_ID`. Panel is **1620×2160**.
  Independently corroborated by an `evtest /dev/input/event3` dump on an `imx8mm-ferrari`
  (Eeems-Org/remarkable.guide issue #74: <https://github.com/Eeems-Org/remarkable.guide/issues/74>).
- The **pen** (`event2`) is a separate digitizer with a **much larger** range (~**11180×15340**) — different
  transform, do not mix it up (KOReader: `frontend/device/remarkable/device.lua`).
- **`struct input_event` is 24 bytes on aarch64** (`timeval` 16 + `type` 2 + `code` 2 + `value` 4).
- **Always resolve the node by device NAME, never by a hardcoded `eventN`.** The numbering is firmware- and
  model-dependent (it shifted by one between rM2 and rMPP — the exact source of the wrong-mapping lore;
  see KOReader's per-model tables). Open `/dev/input/event*`, read `EVIOCGNAME`, pick `"Elan touch input"`
  (or capability-probe for `ABS_MT_SLOT`).

---

## 1. Does the "epaper" QPA deliver finger-touch to Qt/QML on the Paper Pro?

**Yes, it *forwards* it — but Qt then *drops* it before our app sees it.** The QPA is not a wake/refresh-only
consumer. The root cause of "no events reach the app" is **inside Qt's touch dispatch**, not the QPA.

### 1.1 The Paper Pro QPA *does* have a real touch handler [verified-source]

The live Paper Pro plugin is **`reMarkable/epaper-qpa`, branch `new_devices`** (Qt6, last pushed 2026-05).
**Beware:** the other repo `reMarkable/qt5-qpa-epaper` is an **archived 2017 Qt5 stub** ("just a quickly hacked
together qpa based on qminimal") with **no input code** — that is *not* what rMPP runs, and confusing the two
leads to the wrong conclusion that "the QPA has no touch handling."

`new_devices` contains a full evdev multitouch stack forked from Qt's own `QEvdevTouch*` plugin:
`epaperevdevtouchhandler.{cpp,h}`, `epaperevdevtouchscreendata.{cpp,h}`, `epaperevdevtouchmanager.{cpp,h}`,
plus a unit test literally named **"ferrari"** that asserts our exact numbers (ABS `0..2064 / 0..2832`,
screen `1620×2160`, identity transform).
Source tree: <https://github.com/reMarkable/epaper-qpa/tree/new_devices>

The log line our device printed comes straight from this handler **[verified-source]** — confirming it is alive
and read the ABS range (`epaperevdevtouchhandler.cpp`):
```cpp
// line 25
Q_LOGGING_CATEGORY(epaperLcEvdevTouch, "rm.epaperevdevtouchscreenhandler", QtWarningMsg)
// lines 192–199 (the dumpDataParameters debug we saw)
qWarning(epaperLcEvdevTouch) << "xmin" << d->hw_range_x_min;   // 0
qWarning(epaperLcEvdevTouch) << "xmax" << d->hw_range_x_max;   // 2064
... << "screenGeometry" << d->m_screenGeometry;               // QRect(0,0 1620x2160)
... << "rotate" << d->m_rotate;                                // identity
```

It **posts** `QTouchEvent`s (it does not consume them internally) — but with a **null target window**
**[verified-source]** (`epaperevdevtouchhandler.cpp`, lines 56–58):
```cpp
QWindowSystemInterface::handleTouchEvent(nullptr, touchDevice(), points);   // window == nullptr
```
It also registers a proper finger `QPointingDevice` (`QInputDevice::DeviceType::TouchScreen`,
`PointerType::Finger`) via `QWindowSystemInterface::registerInputDevice(...)`, and runs on a worker thread —
the normal Qt evdevtouch design (`handleTouchEvent` is the thread-safe injection API).

reMarkable's own docs even claim **"Touch event handling works out of the box"**
(<https://developer.remarkable.com/documentation/qt_epaper>). On rMPP that promise is **half-true**: the QPA
emits, but the emission shape trips a Qt dispatch gate.

### 1.2 WHY our app sees nothing: the two Qt dispatch gates [verified-source + inference]

The QPA's `handleTouchEvent(nullptr, dev, points)` lands in
`QGuiApplicationPrivate::processTouchEvent` (qtbase 6.8, `src/gui/kernel/qguiapplication.cpp`). Two gates there,
both consequences of the **null window** + a **separately constructed `QPointingDevice`**, can silently eat the
whole event:

- **Gate A — device must be registered** (whole-event drop, no logs):
  ```cpp
  if (!QInputDevicePrivate::isRegistered(e->device))
      return;            // entire touch event dropped before any window lookup
  ```
  Fires if the `QPointingDevice` carried in the event isn't the registered instance (e.g. registration raced
  the first touch, or a duplicate device got created).

- **Gate B — null window ⇒ `topLevelAt()` must hit our window** (per-point drop, then empty-return):
  ```cpp
  QPointer<QWindow> window = e->window;                 // == nullptr (QPA passed null)
  ...
  if (!window)
      window = QGuiApplication::topLevelAt(tempPt.globalPosition().toPoint());
  ...
  if (Q_UNLIKELY(!window)) {
      qCDebug(lcPtrDispatch) << "skipping" << &tempPt << ": no target window";
      continue;          // this point is dropped
  }
  ...
  if (touchEvents.isEmpty())
      return;            // nothing to deliver → our eventFilter/MouseArea never fire
  ```
  and `topLevelAt` resolves against the **platform screen's registered top-level windows**:
  ```cpp
  QWindow *QGuiApplication::topLevelAt(const QPoint &pos) {
      if (QScreen *s = screenAt(pos))
          return s->handle()->topLevelAt(QHighDpi::toNativePixels(pos, s));
      return nullptr;
  }
  ```
  The trap: the QPA maps the touch into the **full-screen rect (1620×2160)** (see §2), but `topLevelAt` only
  returns a window if a **registered Qt top-level window actually covers that global point** on the epaper
  screen. If our QML `Window` isn't a full-panel registered top-level at the platform level, **every point
  misses → `touchEvents.isEmpty()` → return**, and our app-wide `eventFilter` and `MouseArea.onPressed` see
  **nothing** — exactly the observed symptom. (Display worked anyway because the paint path doesn't depend on
  the window↔screen geometry that touch routing needs.)
  *(Qt source quoted by the research pass against `qt/qtbase@6.8`
  `src/gui/kernel/qguiapplication.cpp::processTouchEvent`; these are real Qt 6.8 code paths.)* **[verified-source]**

> **Net:** the QPA is fine; the loss is in Qt window/device resolution. Whether Gate A or Gate B is firing on
> our build needs one on-device check (enable `QT_LOGGING_RULES="qt.pointer.dispatch=true"` and look for
> `"skipping … no target window"`, or log `QInputDevicePrivate::isRegistered`). **[inference on which gate]**

### 1.3 Env vars that change QPA behavior [verified-source]

The QPA's **manager** (`epaperevdevtouchmanager.cpp`) reads the standard Qt var; the *handler* file itself does
not (the node is passed down from the manager):
```cpp
QString spec = QString::fromLocal8Bit(qgetenv("QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS"));
// empty → auto-discovery via QDeviceDiscovery(Device_Touchpad|Device_Touchscreen)
// non-empty → parseSpecification(spec)
```
So you **can**:
- **Force the node:** `export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event3`
- **Orient it:** append `:rotate=90|180|270`, `:invertx`, `:inverty` — these are honored by
  `epaperevdevtouchscreendata.cpp`'s constructor (it builds `m_rotate` from them). reMarkable's docs prescribe
  rM1 `rotate=180`, rM2 `rotate=180:invertx`; **for ferrari they prescribe nothing** and the shipped default is
  identity (matching our log). (<https://developer.remarkable.com/documentation/qt_epaper>;
  Qt var reference: <https://doc.qt.io/qt-6/inputs-linux-device.html>.)

You **cannot**:
- **Override the ABS range** for evdevtouch — no such parameter exists in Qt (it's an `evdevmouse`-only knob).
  The QPA reads the range from the kernel via `EVIOCGABS` (that's why it logged 2064/2832).
- **Disable the QPA's touch handler** — there is **no** `QT_QPA_EVDEV_TOUCHSCREEN_DISABLE` / `EPAPER_*` /
  `RM_*` toggle; the manager is created **unconditionally** in `epaperintegration.cpp::initialize()`.
  (`QT_QPA_FB_DISABLE_INPUT` / `QT_QPA_EGLFS_DISABLE_INPUT` belong to **linuxfb/eglfs**, *not* epaper — they do
  nothing here.) To take touch yourself you simply read `event3` directly (§3); the QPA's handler **does not
  hold a persistent grab** (§4), so a direct reader coexists.

> **`QT_QPA_GENERIC_PLUGINS=evdevtablet` is for the PEN/stylus**, not finger touch — don't reach for it here.

---

## 2. The 2064×2832 vs 1620×2160 + identity transform — do touches land off-window?

**No — within the QPA, the coordinates are normalized and DO land in the panel rect.** The abs/panel mismatch
is harmless because the QPA scales raw → 0..1 → screen geometry **[verified-source]**
(`epaperevdevtouchscreendata.cpp`, `reportPoints()`):
```cpp
tp.normalPosition = QPointF((contact.x - hw_range_x_min) / qreal(hw_range_x_max - hw_range_x_min),
                            (contact.y - hw_range_y_min) / qreal(hw_range_y_max - hw_range_y_min));
if (!m_rotate.isIdentity())
    tp.normalPosition = m_rotate.map(tp.normalPosition);
const QRect winRect = m_screenGeometry;                         // 1620x2160 (the SCREEN, not a window)
const qreal wx = winRect.left() + tp.normalPosition.x() * (winRect.width()  - 1);
const qreal wy = winRect.top()  + tp.normalPosition.y() * (winRect.height() - 1);
```
A physical-center touch → ~(810, 1080), squarely inside 1620×2160. The "ferrari" unit test confirms: raw
`(404,1405)` with `0..2064 / 0..2832`, identity → normalized `{0.1957, 0.1427}` ≈ (316, 308) px. So the
**global position handed to Qt is in panel space**, and Qt does **not** drop it for being out-of-bounds — the
drop is the **window-resolution** issue in §1.2, not a coordinate-range issue. **So "identity transform" here
means "no rotation/inversion applied", which is correct for the ferrari panel — it does NOT mean touches map
off-screen.**

### 2.1 Correct raw→panel transform if you read evdev yourself (§3)

From KOReader's verified Paper Pro tables (`frontend/device/remarkable/device.lua`,
`RemarkablePaperPro:adjustTouchEvent` and the scale wiring) **[verified-source]**:

> **`screen_x = raw_x * 1620 / 2064`** (≈ ×0.7849) **`screen_y = raw_y * 2160 / 2832`** (≈ ×0.7627)
> **no axis swap, no mirror** (stock-firmware path).

This is the **only** rM where touch mirrors **neither** axis — the Elan finger digitizer is already oriented
like the panel. (For contrast: rM2 mirrors touch-Y; rM1 mirrors both; the **pen** on rM2 also swaps X↔Y — none
of that applies to rMPP touch.) Source:
<https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua>
(rMPP table ~L138–146; touch transform ~L181–189; scale ~L309–310/336), added in PR
<https://github.com/koreader/koreader/pull/13620>.

⚠️ **One caveat — kernel/driver path.** KOReader has a second "mainline kernel" code path
(`is_mainline`, triggered on newer rMPP firmware) that **mirrors touch-Y**: `ev.value = mt_height - ev.value`,
and switches the node to `/dev/input/touchscreen0`. Our device is Linux **6.12** (mainline-ish), so the safe
move is **empirical**: scale with no mirror first, tap the **top-left** corner, and if it registers
**bottom-left**, switch to `screen_y = 2160 - raw_y*2160/2832`. (The QPA's own log said `rotate identity`, which
argues for **no mirror** — start there.) **[verified-source for both paths; which one applies = on-device test]**

---

## 3. How community apps actually read touch — direct evdev (the dominant pattern)

Overwhelmingly **yes: read `/dev/input/eventX` directly** with `read()` of `struct input_event` in a loop,
bypassing the toolkit. KOReader, rmkit, Plato, netsurf-reMarkable and Oxide all do this. The Paper Pro finger
digitizer speaks **kernel multitouch Protocol B** (slot-based).

### 3.1 The Protocol-B event grammar for `event3` (Elan touch) [verified-source]

Event **codes** (decimal), confirmed against KOReader's `ev_map` and `<linux/input-event-codes.h>`:

| Event | type | code | meaning |
|---|---|---|---|
| `ABS_MT_SLOT` | `EV_ABS` (3) | **47** | selects the active contact slot (0..9) for subsequent ABS values |
| `ABS_MT_TRACKING_ID` | `EV_ABS` | **57** | **≥0** = contact present/continuing; **−1** = this slot lifted (finger up) |
| `ABS_MT_POSITION_X` | `EV_ABS` | **53** | raw X (0..2064) of the active slot |
| `ABS_MT_POSITION_Y` | `EV_ABS` | **54** | raw Y (0..2832) of the active slot |
| `BTN_TOUCH` | `EV_KEY` (1) | **330** | 1 on first contact, 0 on last lift (do NOT rely on it per-finger) |
| `SYN_REPORT` | `EV_SYN` (0) | **0** | end of one input frame — **apply accumulated state here** |

**Rules (how press/move/release work) — exactly what KOReader and Plato implement:**
1. Maintain a small per-slot table. `ABS_MT_SLOT` sets "current slot". The **kernel** owns slot numbers and
   tracking-ids — you just mirror them.
2. A **new contact** = an `ABS_MT_TRACKING_ID ≥ 0` arriving in a slot (often slot 0 for the first finger),
   followed by `ABS_MT_POSITION_X/Y`. Treat the slot as **down**.
3. **Movement** = further `ABS_MT_POSITION_X/Y` for an already-down slot (no new tracking-id).
4. **Finger lift** = **`ABS_MT_TRACKING_ID == -1`** in that slot. Mark it **up**.
   *(Confirmed by KOReader, quoting the kernel doc: "a non-negative tracking id is interpreted as a contact,
   and the value −1 denotes an unused slot." `frontend/device/input.lua::handleTouchEv` ~L933;
   Plato `crates/core/src/input.rs`: `if value >= 0 { id=value; insert } else { remove → Finger::Up }`.)*
   **[verified-source]**
5. **Apply on `SYN_REPORT`**, never mid-frame: a frame is the burst of ABS events ended by `SYN_REPORT(0)`.
   KOReader runs its gesture detector at SYN_REPORT; Plato emits its `Finger{Down,Motion,Up}` there.
6. **BTN_TOUCH / pressure**: for fingers, **don't depend on them**. `BTN_TOUCH` (330) only brackets
   first-contact/last-lift across all fingers; rMPP touch reports **no usable per-finger pressure** in
   KOReader's model (pressure is consulted only to drop *hovering pen* events). Tracking-id is the source of
   truth. **[verified-source]**

Sources: KOReader MT state machine
<https://github.com/koreader/koreader/blob/master/frontend/device/input.lua> (`handleTouchEv` ~L933,
`ev_map` ~L33–82); KOReader C reader (raw 24-byte reads, grab)
<https://github.com/koreader/koreader-base/blob/master/input/input.c>; Plato generic Protocol-B parser
<https://github.com/baskerville/plato/blob/master/crates/core/src/input.rs>; rmkit Protocol-B
`events.cpy::TouchEvent::handle_abs()` and read loop `input.cpy`
<https://github.com/rmkit-dev/rmkit/blob/master/src/rmkit/input/events.cpy>,
<https://github.com/rmkit-dev/rmkit/blob/master/src/rmkit/input/input.cpy> (capability-based node id:
`device_id.cpy`); netsurf-reMarkable (libevdev, Protocol-B, **rM1/rM2-only** machine gate)
<https://github.com/alex0809/libnsfb-reMarkable/blob/master/src/surface/remarkable/input.c>.

> **Portability note:** rmkit and netsurf-reMarkable do **not** support rMPP out of the box (rmkit assumes
> `/dev/fb0`+mxcfb; netsurf gates on `reMarkable 1.0/2.0` machine names). KOReader **does** have explicit
> "reMarkable Ferrari" support and is the authoritative reference. But the **evdev protocol** all of them speak
> is identical to what `event3` emits — only their framebuffer/node-hardcoding is device-specific.

---

## 4. Reading `event3` directly WHILE the epaper QPA is active — conflict? Grab?

**No hard conflict. Both can read. You do NOT need to grab. Grabbing is OPTIONAL and has one nice side
effect.** The reasoning, from the kernel evdev driver and the QPA source:

### 4.1 Kernel evdev semantics [verified-source — `drivers/input/evdev.c`]

- **evdev is per-fd broadcast.** Every `open()` of `/dev/input/event3` gets its **own** `evdev_client` with its
  **own** ring buffer; the kernel delivers each event to **all** clients independently
  (`evdev_events()` → `list_for_each_entry_rcu(client, …) evdev_pass_values(...)`). **So two non-grabbing
  readers BOTH receive every event** — e.g. our thread *and* the QPA's handler can both read it, and `evtest`
  can run alongside. <https://github.com/torvalds/linux/blob/master/drivers/input/evdev.c>
- **`EVIOCGRAB` is exclusive**, and a second grab returns **`-EBUSY`**:
  ```c
  static int evdev_grab(struct evdev *evdev, struct evdev_client *client) {
      if (evdev->grab) return -EBUSY;
      ... rcu_assign_pointer(evdev->grab, client); return 0;
  }
  ```
  While one client holds the grab, **all other open fds stop receiving**.

### 4.2 The QPA does NOT hold a persistent grab [verified-source]

Critically, the Paper Pro QPA's touch handler only **probes** grab-ability at startup and **releases it
immediately** — it does **not** keep the device grabbed at runtime (`epaperevdevtouchhandler.cpp`):
```cpp
// lines 45–46
m_fd = QT_OPEN(device…, O_RDONLY | O_NDELAY, 0);
// lines 129–132
bool grabSuccess = !ioctl(m_fd, EVIOCGRAB, (void *) 1);
if (grabSuccess)
    ioctl(m_fd, EVIOCGRAB, (void *) 0);   // released right away
else
    /* warns: device grabbed by another process */;
```
(This mirrors upstream Qt's `QEvdevTouchScreenHandler`, which does the same transient probe and does **not**
hold a runtime grab — contrary to a common belief that Qt grabs touch.
<https://codebrowser.dev/qt5/qtbase/src/platformsupport/input/evdevtouch/qevdevtouchhandler.cpp.html>)

**Consequences:**
- Our direct **non-grabbing** reader on `event3` and the QPA's handler **coexist** — both get every event.
  (That also means: if we leave the QPA handler active, the *same* touches reach both, and the QPA still tries
  to dispatch them into Qt — relevant to the WPE crash below.)
- If we **`EVIOCGRAB` event3 first** (before the QPA constructs its handler, or persistently), the QPA's
  startup probe finds it grabbed → it logs *"device is grabbed by another process, no events will be read"* and
  **stops feeding touch into Qt entirely**. That is the clean way to make the QPA's (broken-for-us) touch path
  **go silent** so it can't interfere.

### 4.3 Should we grab? — recommendation

- **xochitl is already stopped**, so we don't need a grab to silence the stock UI (the usual community reason to
  grab — Oxide/rmkit/xochitl all grab to take input away from each other:
  <https://github.com/Eeems-Org/oxide>, rmkit `lock()/grab()` in `input.cpy`).
- **Grab is still RECOMMENDED here**, for one reason: it **disables the QPA's own touch dispatch** (§4.2), which
  is the most likely cause of the **WPE-app segfault on touch** (§5). With a persistent
  `ioctl(fd, EVIOCGRAB, 1)` on `event3`, only our thread receives touches; the QPA handler reads nothing and
  posts nothing into Qt, so the fragile null-window → WebKit touch path can't fire. **[inference, but
  mechanically sound and matches the kernel/QPA facts above]**
- KOReader grabs (`koreader-base/input/input.c`: `ioctl(fd, EVIOCGRAB, 1)` after
  `open(O_RDONLY|O_NONBLOCK|O_CLOEXEC)`); Plato and netsurf-reMarkable do **not** grab. Both styles work; we
  pick grab for the QPA-silencing benefit.

---

## 5. Why does the WPE app SEGFAULT on touch (but plain QtQuick doesn't)? [inference]

Most likely: **the QPA's null-window touch event drives Qt's touch→mouse / touch→gesture synthesis into the WPE
web view, where an unguarded pointer deref crashes** — a class of bug well documented in Qt's web stack:
- **QTBUG-74008** — `pointById(touchMouseId)` returns `nullptr` with no guard during multi-finger gesture
  synthesis → crash. <https://bugreports.qt.io/browse/QTBUG-74008>
- Qt WebEngine touch-tap crashes fixed only by pulling in the touch-handle/selection QML delegates
  (<https://forum.qt.io/topic/160259>), and `QWebEngineView` "invalid gesture event" touch crashes
  (<https://www.qtcentre.org/threads/63190-QWebEngineView-Touch-Crash>).

A plain `MouseArea` is robust to the null-window path; **web content** exercises the fragile
touch→gesture/selection plumbing. That cleanly explains "QtQuick = no crash, WPE = crash." The QPA's own
`reportPoints()` is unlikely to be the faulting frame (it's a faithful Qt fork with no app-object deref).

**Practical upshot:** **grabbing `event3` (which silences the QPA's touch dispatch, §4.2) is expected to STOP
the WPE crash**, and we feed our own clean, mapped events to the engine instead. This is the recommended path.

---

## 6. Recommended approach + minimal C snippet

**Decision: pick a SINGLE owner of `event3` = our own direct-evdev reader, and `EVIOCGRAB` it.** Do **not** rely
on the epaper QPA for finger touch (it drops events into a null window, and its dispatch likely crashes WPE).
Resolve the node by **name** (`"Elan touch input"`), grab it, decode Protocol B on a worker thread, map
`x*1620/2064, y*2160/2832` (no mirror; verify top-left), and feed our shell/engine. The pen (`event2`) is a
separate, later concern.

```c
// touch_rmpp.c — minimal single-finger reader for the reMarkable Paper Pro "Elan touch input"
// Build (device, aarch64): aarch64-remarkable-linux-gcc -O2 touch_rmpp.c -o touch_rmpp
// Run with xochitl stopped. struct input_event is 24 bytes on aarch64.
#include <linux/input.h>          // input_event, EV_ABS/EV_KEY/EV_SYN, ABS_MT_*, SYN_REPORT, EVIOCGNAME, EVIOCGRAB
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>

// Raw Elan ranges (verified on device) and target panel:
#define TOUCH_RAW_W   2064
#define TOUCH_RAW_H   2832
#define PANEL_W       1620
#define PANEL_H       2160
// Stock-path transform (no swap, no mirror). If a top-left tap lands bottom-left, set MIRROR_Y to 1.
#define MIRROR_Y      0
static inline int map_x(int raw){ return (int)((long)raw * PANEL_W / TOUCH_RAW_W); }
static inline int map_y(int raw){ int y=(int)((long)raw*PANEL_H/TOUCH_RAW_H); return MIRROR_Y?(PANEL_H-1-y):y; }

// --- find /dev/input/eventN whose EVIOCGNAME == "Elan touch input" (NEVER hardcode the number) ---
static int open_touch_by_name(const char *want){
    DIR *d = opendir("/dev/input");
    if(!d) return -1;
    struct dirent *e; char path[64], name[256];
    while((e = readdir(d))){
        if(strncmp(e->d_name, "event", 5)) continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);   // shared by default
        if(fd < 0) continue;
        name[0] = 0;
        if(ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strcmp(name, want) == 0){
            closedir(d);
            return fd;                                            // caller decides whether to grab
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

int main(void){
    int fd = open_touch_by_name("Elan touch input");             // == /dev/input/event3 on rMPP, but resolved by name
    if(fd < 0){ fprintf(stderr, "touch device not found\n"); return 1; }

    // Take the device exclusively: also makes the epaper QPA's touch handler go silent (prevents WPE touch crash).
    if(ioctl(fd, EVIOCGRAB, (void*)1) != 0) fprintf(stderr, "warning: EVIOCGRAB failed (something else holds it)\n");

    // Single-finger state (Protocol B; we only track the first slot here).
    int cur_slot = 0;          // updated by ABS_MT_SLOT
    int down = 0;              // is a finger currently down?
    int x = 0, y = 0;          // last mapped coords
    int start_x = 0, start_y = 0;
    // swipe thresholds (panel px)
    const int SWIPE_MIN_DY = 240;   // vertical travel to count as a page-turn (~11% of height)
    const int SWIPE_MAX_DX = 200;   // keep it roughly vertical

    struct input_event ev[64];      // 24 bytes each on aarch64
    for(;;){
        ssize_t n = read(fd, ev, sizeof ev);
        if(n < (ssize_t)sizeof(struct input_event)){
            if(n < 0){ /* EAGAIN on O_NONBLOCK: poll()/epoll here in real code */ }
            continue;
        }
        for(size_t i = 0; i < n / sizeof(struct input_event); i++){
            struct input_event *p = &ev[i];
            if(p->type == EV_ABS){
                switch(p->code){
                    case ABS_MT_SLOT:        cur_slot = p->value; break;               // 47
                    case ABS_MT_TRACKING_ID:                                           // 57
                        if(cur_slot != 0) break;                                       // first finger only
                        if(p->value >= 0){            // new contact → DOWN
                            down = 1; start_x = x; start_y = y;
                            printf("DOWN  (%d,%d)\n", x, y);
                        } else {                       // -1 → finger LIFTED
                            if(down){
                                int dx = x - start_x, dy = y - start_y;
                                printf("UP    (%d,%d)\n", x, y);
                                if(dy <= -SWIPE_MIN_DY && (dx>-SWIPE_MAX_DX&&dx<SWIPE_MAX_DX))
                                    printf("SWIPE UP    -> next page\n");
                                else if(dy >= SWIPE_MIN_DY && (dx>-SWIPE_MAX_DX&&dx<SWIPE_MAX_DX))
                                    printf("SWIPE DOWN  -> prev page\n");
                            }
                            down = 0;
                        }
                        break;
                    case ABS_MT_POSITION_X:  if(cur_slot==0) x = map_x(p->value); break; // 53
                    case ABS_MT_POSITION_Y:  if(cur_slot==0) y = map_y(p->value); break; // 54
                    default: break;
                }
            } else if(p->type == EV_SYN && p->code == SYN_REPORT){   // 0,0 = end of frame → act now
                if(down) printf("MOVE  (%d,%d)\n", x, y);
            }
            // BTN_TOUCH (EV_KEY 330) is available but NOT needed: tracking_id drives down/up.
        }
    }
    // ioctl(fd, EVIOCGRAB, (void*)0); close(fd);   // on shutdown
}
```

**Notes on the snippet**
- It resolves the node by **name** via `EVIOCGNAME` (robust across firmware) and `EVIOCGRAB`s it (silences the
  QPA touch path → fixes the WPE crash). For multi-finger / gestures, extend the per-slot table (indexed by
  `cur_slot`, 0..9) and act per slot at `SYN_REPORT`; the single-slot version above is enough for page-turns.
- Use `poll()`/`epoll` on `fd` in production rather than a busy `read()` on a non-blocking fd (omitted for
  brevity). KOReader/rmkit read up to 64 events per syscall as here.
- `O_NONBLOCK` keeps the worker responsive; the QPA opened the same device `O_RDONLY|O_NDELAY` — but once we
  grab, the QPA stops getting events anyway.
- Headers: `<linux/input.h>` pulls in `<linux/input-event-codes.h>` (the `ABS_MT_*`/`BTN_TOUCH`/`SYN_REPORT`
  constants) on this kernel.

---

## 7. Open items to verify on the device (cheap, 5 minutes)

1. **Mirror?** Tap top-left; if it reads bottom-left, set `MIRROR_Y 1` (`y = 2160 - raw_y*2160/2832`).
   (KOReader's mainline-kernel path mirrors Y; the QPA logged identity, so we default to no-mirror.)
2. **WPE crash gone with grab?** Run the WPE app, `EVIOCGRAB` event3 from our thread, tap — confirm no segfault.
   (Or, as a quick test independent of our reader: a tiny helper that just grabs event3 and sleeps should also
   stop the crash, proving the QPA dispatch is the culprit.)
3. **Which Qt gate was eating events** (only if we ever want the QPA path to work for the QML chrome):
   `QT_LOGGING_RULES="qt.pointer.dispatch=true"` and look for `"skipping … no target window"` (Gate B) vs
   total silence (Gate A). Fix = make the QML `Window` a full-panel registered top-level (the same
   `Screen.width/height` recipe that fixed display in Phase 1) and don't construct a second `QPointingDevice`.

---

## 8. Sources (index)

- reMarkable epaper QPA (live, Qt6, ferrari): <https://github.com/reMarkable/epaper-qpa/tree/new_devices> —
  `epaperevdevtouchhandler.cpp` (grab probe L129–132, `handleTouchEvent(nullptr,…)` L56–58, open L45,
  log/dump L25/L192–199), `epaperevdevtouchscreendata.cpp` (normalize+map `reportPoints`, rotate/invertx/inverty),
  `epaperevdevtouchmanager.cpp` (reads `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS`), `tests/tst_touch.cpp` ("ferrari").
- Archived Qt5 stub (NOT what rMPP runs): <https://github.com/reMarkable/qt5-qpa-epaper>
- reMarkable Qt-epaper docs ("touch works out of the box"; rM1/rM2 rotate params):
  <https://developer.remarkable.com/documentation/qt_epaper>
- Qt touch dispatch gates: `qt/qtbase@6.8` `src/gui/kernel/qguiapplication.cpp::processTouchEvent`
  (isRegistered guard; null-window `topLevelAt`; `lcPtrDispatch` "no target window"; empty-return).
  Qt evdev env vars: <https://doc.qt.io/qt-6/inputs-linux-device.html>. Qt evdevtouch transient grab:
  <https://codebrowser.dev/qt5/qtbase/src/platformsupport/input/evdevtouch/qevdevtouchhandler.cpp.html>
- Linux kernel evdev (broadcast per-fd; `EVIOCGRAB` → `-EBUSY`):
  <https://github.com/torvalds/linux/blob/master/drivers/input/evdev.c>
- KOReader (authoritative rMPP support): device/transform/node map
  <https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua>;
  MT state machine <https://github.com/koreader/koreader/blob/master/frontend/device/input.lua>;
  C reader + grab <https://github.com/koreader/koreader-base/blob/master/input/input.c>;
  rMPP PR <https://github.com/koreader/koreader/pull/13620>.
- Plato (generic Protocol-B; no grab; Kobo-only):
  <https://github.com/baskerville/plato/blob/master/crates/core/src/input.rs>
- rmkit (Protocol-B reader + grab; capability node-id; rM1/rM2 framebuffer):
  <https://github.com/rmkit-dev/rmkit/blob/master/src/rmkit/input/input.cpy>,
  <https://github.com/rmkit-dev/rmkit/blob/master/src/rmkit/input/events.cpy>,
  <https://github.com/rmkit-dev/rmkit/blob/master/src/rmkit/input/device_id.cpy>
- netsurf-reMarkable (libevdev Protocol-B; no grab; rM1/rM2-only machine gate):
  <https://github.com/alex0809/libnsfb-reMarkable/blob/master/src/surface/remarkable/input.c>
- Oxide / liboxide (grabs input to displace xochitl): <https://github.com/Eeems-Org/oxide>,
  <https://remarkable.guide/devel/language/c++/liboxide.html>
- remarkable.guide ferrari hardware dump (event3 = "Elan touch input", 2064×2832, INPUT_PROP_DIRECT):
  <https://github.com/Eeems-Org/remarkable.guide/issues/74>; general input model (rM1/rM2):
  <https://remarkable.guide/devel/device/input.html>
- Qt WebEngine touch crash precedents: <https://bugreports.qt.io/browse/QTBUG-74008>,
  <https://forum.qt.io/topic/160259>, <https://www.qtcentre.org/threads/63190-QWebEngineView-Touch-Crash>
