# reMarkable Paper Pro — software-development reference

Research compiled **2026-06-26** from community + primary sources, for native development on the
**reMarkable Paper Pro** (codename **"ferrari"** / **"reMarkable Ferrari"**, a.k.a. **rMPP**):
**i.MX8M Mini, aarch64 (4× Cortex-A53), GPU on die (Vivante GC7000 UltraLite) but NO driver in the stock OS
(CPU-only in practice), COLOR E Ink Gallery 3 / ACeP2 panel ~1620×2160,
Codex Linux** (Yocto **scarthgap**, **glibc 2.39**), **BusyBox** userland, system **Qt 6.10.3** (OS 3.28;
rmweb links it dynamically and bundles only the missing qtvirtualkeyboard module, cross-built against
the SDK's Qt 6.8.2) with a reMarkable
**"epaper" QPA** plugin. Root SSH over USB ethernet (`root@10.11.99.1`).

> **How to read this doc.** Every claim cites a source URL. Facts are tagged:
> **[PP]** verified/specific to Paper Pro · **[rM1/rM2]** older grayscale devices (for contrast — usually
> *does not* apply to PP) · **[general]** general Linux/WebKit/Memfault/Yocto knowledge (very likely applies,
> not PP-proven) · **[verify]** needs an on-device check (command given). The device is **BusyBox** — use
> `head -n N` (not `head -N`), `ps -ef`.
>
> **Biggest single fact:** rMPP is a *different world* from rM1/rM2. It drives the panel via **DRM/KMS
> (`/dev/dri/card0`, `imx-drm`)** — there is **no `/dev/fb0`, no `mxc_epdc`, no `MXCFB_SEND_UPDATE` ioctl**.
> So rm2fb, libremarkable, rmkit, reStream, etc. **do not apply**. You present through reMarkable's **Qt epaper
> stack** (`libepaper.so` + `libqsgepaper.so`), or reverse-engineer DRM yourself (undocumented — see §3.5).
> ([remarkable.guide display devel — a stub](https://remarkable.guide/devel/device/display.html);
> on-device `drm_info`: [Eeems-Org/remarkable.guide#74](https://github.com/Eeems-Org/remarkable.guide/issues/74))

---

## Table of contents
1. [Existing browsers / WebKit / Chromium on reMarkable](#1-existing-browsers--webkit--chromium-on-remarkable)
2. [The epaper Qt QPA plugin: input, geometry, env vars](#2-the-epaper-qt-qpa-plugin-input-geometry-env-vars)
3. [E-ink refresh control on the Paper Pro](#3-e-ink-refresh-control-on-the-paper-pro)
4. [JavaScriptCore / JIT and W^X](#4-javascriptcore--jit-and-wx)
5. [Lifecycle: rootfs, OTA, xochitl, suspend, and crash-reboot (Memfault/watchdog)](#5-lifecycle-rootfs-ota-xochitl-suspend-and-crash-reboot-memfaultwatchdog)
6. [Corrections to prior project assumptions](#6-corrections-to-prior-project-assumptions)
7. [Source index](#7-source-index)

---

## 1. Existing browsers / WebKit / Chromium on reMarkable

**Scope note.** rM1/rM2 = 32-bit ARMv7 i.MX6/i.MX7, no GPU, **grayscale** Carta E Ink. **Paper Pro ("ferrari")**
= aarch64 i.MX8M-class, GPU on die but no stock-OS driver (CPU-only in practice), **color Gallery 3 / ACeP2**,
with a different system layout (overlay
filesystems, secure boot, system Qt 6.10.3 on current 3.28 builds + "epaper" QPA). Community tooling for rM1/rM2
does **not** transfer unchanged. ([Toltec discussion #910](https://github.com/toltec-dev/toltec/discussions/910);
[remarkable.guide FAQ](https://remarkable.guide/faqs.html))

### 1.1 The inventory (all generations)
There is effectively **one** real native HTML browser in the community — **NetSurf**. Everything else is a
document/EPUB reader, a *desktop-side* extension, or a remote/VNC trick. **No Chromium, Electron, CEF, or WPE
WebKit browser ships or runs natively on any reMarkable** as of mid-2026.
([awesome-reMarkable](https://github.com/reHackable/awesome-reMarkable);
[toltec-dev.org/stable](https://toltec-dev.org/stable/))

### 1.2 NetSurf (the only native browser)
- Repos: wrapper [alex0809/netsurf-reMarkable](https://github.com/alex0809/netsurf-reMarkable) · engine fork
  [alex0809/netsurf-base-reMarkable](https://github.com/alex0809/netsurf-base-reMarkable) · framebuffer fork
  [alex0809/libnsfb-reMarkable](https://github.com/alex0809/libnsfb-reMarkable).
- **Engine = NetSurf's own lightweight HTML/CSS engine (NOT WebKit/Blink)**; "very limited JavaScript
  support… brilliant for lightweight web-browsing, especially with FrogFind."
  ([repo](https://github.com/alex0809/netsurf-reMarkable);
  [akselmo.dev](https://akselmo.dev/posts/netsurf-on-remarkable-2/))
- **Rendering to e-ink = NetSurf's framebuffer frontend via `libnsfb`** — a pure **CPU/software rasterizer**,
  no GPU. The reMarkable fork adds device screen-draw + input glue.
  ([libnsfb-reMarkable](https://github.com/alex0809/libnsfb-reMarkable);
  [netsurf-dev fb thread](https://www.mail-archive.com/netsurf-dev@netsurf-browser.org/msg03798.html))
- **Generation support:** repo targets **rM1 + rM2** (Toltec `opkg install netsurf`); **Paper Pro is NOT in
  this repo**, and the repo was **archived (June 2026)**.
  ([repo](https://github.com/alex0809/netsurf-reMarkable)) **[rM1/rM2]**
- **BUT NetSurf does run on the Paper Pro** — via a *different* loader stack: **XOVI + rm-appload + a
  qtfb-shim** (run `nsfb` with `"QTFB_SHIM_MODEL":"0"`, `"QTFB_SHIM_INPUT_MODE":"NATIVE"`; use the
  aarch64 `extensions-aarch64.zip`). JS off by default; cookies/logins don't persist across restarts.
  ([akselmo.dev](https://akselmo.dev/posts/netsurf-on-remarkable-2/);
  [Nilorea XOVI rM1/2/PP](https://www.nilorea.net/2025/08/11/latest-rmhacks-with-xovi-for-remarkable-1-2-paper-pro/)) **[PP]**

### 1.3 Has anyone run WPE WebKit / a modern engine on the Paper Pro? — **No public port found**
- **No repo, thread, or blog** shows WPE WebKit, Chromium, or any modern engine running on rMPP from anyone
  other than this project. ([wpewebkit.org](https://wpewebkit.org/);
  [WebPlatformForEmbedded/WPEWebKit](https://github.com/WebPlatformForEmbedded/WPEWebKit))
- **Igalia published nothing about reMarkable / e-ink / Paper Pro / ferrari** in their WebKit periodicals or
  `blogs.igalia.com` (their WPE material is set-top-box/signage/automotive, generic-embedded).
  ([WebKit Igalia Periodical](https://blogs.igalia.com/webkit/blog/2026/wip-56/);
  [igalia.com/project/wpe](https://www.igalia.com/project/wpe))
- → **This project's "WPE WebKit + Mesa software EGL (softpipe) + Skia CPU → epaper QPA" appears to be the
  first WPE-on-rMPP.** The generic WPE-on-embedded reuse (meta-webkit, software rendering) applies, but no one
  has applied it to rMPP publicly.

### 1.4 The actual third-party app stack on the Paper Pro (closest prior art for display)
The community reaches the color e-ink panel **without stopping xochitl**, by emulating the old framebuffer and
letting **xochitl drive the waveforms**:
- **XOVI** — LD_PRELOAD hook/extension framework; works on reMarkable **OS 3.20+**.
  ([asivery/xovi](https://github.com/asivery/xovi);
  [Nilorea](https://www.nilorea.net/2025/08/11/latest-rmhacks-with-xovi-for-remarkable-1-2-paper-pro/))
- **qtfb** — a Qt framebuffer XOVI extension. ([asivery/qtfb](https://github.com/asivery/qtfb))
- **rmpp-qtfb-shim** — "emulates the rM1 framebuffer and input devices on rMPP thanks to qtfb"; lets legacy
  apps (recompiled aarch64) think they have an rM1 framebuffer.
  ([asivery/rmpp-qtfb-shim](https://github.com/asivery/rmpp-qtfb-shim))
- **rm-appload ("AppLoad")** — XOVI extension to run windowed/fullscreen apps (QML frontends, any-language
  backends over a unix socket); supports rM1/rM2/rMPP/rMPPM.
  ([asivery/rm-appload](https://github.com/asivery/rm-appload))
- **Key architectural contrast:** this stack lets **xochitl manage refresh**; this project instead **stops
  xochitl and drives the epaper QPA / `EPFramebuffer` full-refresh directly** (§3). For *color refresh tuning*
  there is **no community prior art** — KOReader explicitly defers it: refresh is "controlled by xochitl…
  will change in future after qtfb and shim gets ability to control it."
  ([KOReader PR #13620](https://github.com/koreader/koreader/pull/13620))

### 1.5 Chromium / Electron / CEF — none, and why
- **No working Chromium/Electron/CEF port on any reMarkable.** Not in awesome-reMarkable, Toltec, or threads;
  the device is "a writing tablet… not designed to run third-party apps like web browsers," and guides only
  ever load NetSurf. ([Liliputing](https://liliputing.com/lilbits-a-web-browser-for-the-remarkable-2-e-ink-tablet-a-diy-keyboard-phone-made-from-a-program-galaxy-z-flip-and-more/))
- **Why impractical [general]:** no GPU driver in the stock OS → Chromium's renderer/compositor must fall back to slow software GL
  (`--disable-gpu`, `LIBGL_ALWAYS_SOFTWARE=1`); plus heavy V8/Blink memory + JIT cost on a low-power ARM e-ink
  device. (No one published an actual rM Chromium attempt — this is reasoned from general no-GPU behavior, not
  a documented rM trial.) ([Chrome SW-renderer fallback](https://www.lexo.ch/blog/2026/02/websites-not-loading-in-chrome-or-chromium-based-browsers-how-to-fix-gpu-and-webgl-related-rendering-failures/))
- This is precisely **why NetSurf (no JIT, tiny footprint, CPU framebuffer) is the only browser the community
  ships.** ([repo](https://github.com/alex0809/netsurf-reMarkable))

### 1.6 Reader-style HTML/EPUB viewers (relevant prior art for a *reading* browser)
- **KOReader** — [koreader/koreader](https://github.com/koreader/koreader). Reflowable EPUB/HTML/FB2/PDF/DjVu
  via a CoolReader/crengine fork, **CPU only**. **Paper Pro support is officially merged**
  ([PR #13620, merged 2025-04-21](https://github.com/koreader/koreader/pull/13620);
  [issue #12856](https://github.com/koreader/koreader/issues/12856)): adds a **`remarkable-aarch64`** target,
  recognizes **Color Gallery 3**, handles **touch + pen**, frontlight via `/sys/class/backlight/rm_frontlight/`
  (0–2047). **But it talks to the panel via QTFB + rmpp-qtfb-shim and leaves refresh to xochitl.** Install needs
  **XOVI + QTFB + rmpp-qtfb-shim + LD_PRELOAD**. **This is the best existing Paper Pro reference** (aarch64
  target, Gallery 3 detection, evdev touch/pen handling, frontlight path). **[PP]**
- **Plato** — [LinusCDE/plato](https://github.com/LinusCDE/plato), MuPDF-based reader. **rM1/rM2 only**; on rM2
  "refresh modes… not as granular due to reduced framebuffer access." **No Paper Pro support.**
  ([awesome-reMarkable](https://github.com/reHackable/awesome-reMarkable)) **[rM1/rM2]**

### 1.7 Desktop-side "browser" tooling (not on-device engines)
- **rePub** (Chrome ext, page→ePub) [hafaio/repub](https://github.com/hafaio/repub) / Firefox fork
  [jrockwar/repubfox](https://github.com/jrockwar/repubfox). · **"Read on reMarkable"** official Chrome
  extension ([support.remarkable.com](https://support.remarkable.com/s/article/Read-on-reMarkable-Google-Chrome-Extension)).
  · **USB Web Interface** = device-hosted file-upload UI, not a browser
  ([remarkable.guide](https://remarkable.guide/tech/usb-web-interface.html)). · **VNC/VNSee** renders a remote
  desktop's pixels, not a local engine ([gist](https://gist.github.com/JustSimplyKyle/cf4a8ceff2763d82a2815e5c8c27dc3d)).

### 1.8 Root access (no jailbreak needed)
Paper Pro **Developer Mode** gives root SSH; it disables most of secure boot but **not** disk encryption.
([reMarkable Developer Mode](https://developer.remarkable.com/documentation/developer-mode);
[support.remarkable.com](https://support.remarkable.com/s/article/Developer-mode))

---

## 2. The epaper Qt QPA plugin: input, geometry, env vars

> **Two "epaper" codebases exist and are frequently conflated:**
> 1. **Open-source `epaper-qpa` / `qt5-qpa-epaper`** (reMarkable org, Qt5, "hacked together from qminimal,"
>    archived 2022) — basis of community dev guides.
>    [github.com/reMarkable/epaper-qpa](https://github.com/reMarkable/epaper-qpa),
>    [qt5-qpa-epaper](https://github.com/reMarkable/qt5-qpa-epaper).
> 2. **Closed binary `libepaper.so` + `libqsgepaper.so`** that ships on-device (built against the system Qt —
   >    6.10.3 on current 3.28 builds). reMarkable's
>    current guide targets this: [developer.remarkable.com/documentation/qt_epaper](https://developer.remarkable.com/documentation/qt_epaper).
>
> Behavior can differ; claims are labeled. Device machine strings (`/sys/devices/soc0/machine`):
> **Paper Pro = `reMarkable Ferrari`**, Paper Pro Move = `reMarkable Chiappa`.
> ([KOReader device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua))

### 2.1 How touch and pen reach Qt
- **Touch works out of the box; pen/marker does NOT (in the open QPA).** reMarkable's guide: *"Touch event
  handling works out of the box"* (consume in QML via a plain `MouseArea { onPressed: }`), but *"Handling the
  marker is more involved and not shown here."*
  ([qt_epaper](https://developer.remarkable.com/documentation/qt_epaper)) **[PP]**
- **Touch is delivered through Qt's *stock* evdev stack, not a custom handler.** The open `epaper-qpa`
  instantiates Qt's built-in `new QEvdevTouchManager("EvdevTouch", …)` inside `EpaperIntegration` — so QtQuick
  `MouseArea` and `MultiPointTouchArea` receive events normally (Qt auto-synthesizes mouse from touch). The
  repo's master tree has only *keyboard* evdev files (`epaperevdevkeyboard*`) — **no touch handler source** —
  confirming touch = stock Qt path, keyboard = custom, pen = not handled by the QPA.
  ([epaperintegration.cpp](https://github.com/reMarkable/epaper-qpa/blob/master/epaperintegration.cpp))
- **PEN/marker arrives via Qt's generic `evdevtablet` plugin, separately**, as a **`QTabletEvent`** — only if
  you opt in with **`QT_QPA_GENERIC_PLUGINS=evdevtablet`**. A plain `MouseArea` will **not** see the pen unless
  you handle tablet events (or read evdev directly).
  ([dragly.org — Developing for the reMarkable](https://dragly.org/2017/12/01/developing-for-the-remarkable/))
- **Known limitations:** marker handling is undocumented by reMarkable; only **pure Qt Quick** is supported
  (no Qt Widgets); on SW 3.17+ you must manually deploy `libqsgepaper.so` from the SDK; whether the QPA
  forwards **hover** to QML is **[verify]** (pen reports hover/distance at evdev level — §2.5).
  ([qt_epaper](https://developer.remarkable.com/documentation/qt_epaper))

### 2.2 evdev input-node mapping
**⚠️ There is a documented conflict — resolve by name, not by `eventN`.**

- **KOReader `device.lua` (authoritative source-of-record for the port):** for `RemarkablePaperPro`,
  `event0=buttons`, `event1=hall`, **`event2=PEN`** (`input_wacom`), **`event3=TOUCH`** (`input_ts`).
  ([device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua)) **[PP]**
- **This project's own on-device recon (`docs/device-profile.md`, 2026-06-24):** `event2 = "Elan touch input"
  (touchscreen, symlink touchscreen0)`, `event3 = "Elan marker input" (pen)` — i.e. **the opposite order.**
- **Resolution:** these disagree, so **do not hard-code `event2`/`event3`.** Identify by device **name** /
  `by-path`. On mainline-kernel firmware (rMPP SW ≈ 3.19+) numbered nodes shift anyway; KOReader resolves by
  path: pen `= /dev/input/by-path/platform-30a20000.i2c-event-mouse`, buttons
  `= …platform-30370000.snvs:snvs-powerkey-event`, touch `= /dev/input/touchscreen0`.
  Verify on-device: **`cat /proc/bus/input/devices`** (match the `N:` name → `H: Handlers=eventX`).
  ([device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua)) **[verify]**
- **event4 = Type Folio keyboard** is plausible (matches this project's profile) but **unconfirmed in any
  primary source** — **[verify]**.
- **Pen is Elan-based active stylus, NOT Wacom EMR, on Paper Pro.** KOReader's code comment: *"Wacom (it's not
  Wacom on Paper Pro but it should work)"* — the `input_wacom` field name is kept only for code reuse. The
  "Marker Plus" is battery/inductively-charged, not EMR. reMarkable's kernel ships Elan drivers
  (`elants_i2c`/`TOUCHSCREEN_ELAN`).
  ([device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua);
  [dreeko/remarkable-input-tablet](https://github.com/dreeko/remarkable-input-tablet)) **[PP]**
- **Contrast — rM1/rM2 use a Wacom EMR pen on different nodes:** rM1 pen=event0/touch=event1/buttons=event2;
  rM2 pen=event1/touch=event2/buttons=event0. The rM2 Wacom digitizer exposes ToolPen/ToolRubber + ABS
  X/Y/Pressure/Distance/Tilt.
  ([device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua);
  [libremarkable wiki — Wacom I2C](https://github.com/canselcik/libremarkable/wiki/Reading-from-Wacom-I2C-Digitizer)) **[rM1/rM2]**

### 2.3 Screen geometry, coordinate transforms, rotation
From KOReader `device.lua` for `RemarkablePaperPro`
([device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua)) **[PP]**:
- **Display:** `screen_width = 1620`, `screen_height = 2160`, `display_dpi = 229`.
- **Touch matrix** (higher than display): `mt_width = 2064`, `mt_height = 2832` →
  `mt_scale_x = 1620/2064 ≈ 0.785`, `mt_scale_y = 2160/2832 ≈ 0.763`.
- **Pen matrix:** `wacom_width = 11180`, `wacom_height = 15340` → `wacom_scale = screen/wacom`.
- **Touch transform = pure scale, NO mirror/swap** (unlike rM1 which mirrors both axes, rM2 which mirrors Y).
  *(The code comment says "Mirror X and Y" but the body only multiplies — no mirroring.)*
- **Pen transform = pure scale, NO axis-swap** (unlike rM1/rM2 which swap `ABS_X↔ABS_Y` + invert Y).
  → On Paper Pro, pen and touch share the display's orientation; **no rotation/swap needed.**
  KOReader sets `native_rotation_mode = DEVICE_ROTATED_UPRIGHT`.
- **The documented Qt "cure": size the QML `Window` to `Screen.width`/`Screen.height`** and let the QPA drive
  geometry (don't force it from C++ after creation — that yields a 0×0 first frame → partial update → white
  screen with a fragment). reMarkable's example uses exactly
  `Window { width: Screen.width; height: Screen.height; visible: true }`.
  ([qt_epaper](https://developer.remarkable.com/documentation/qt_epaper)) **[PP]** (matches this project's
  verified Phase-1 finding).
- **Rotation (Qt path) belongs in the env var, not your math.** On rM1/rM2, touch axes are corrected by
  `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` (`rotate=180` / `rotate=180:invertx`). **Paper Pro's correct value is
  not published**; since KOReader needs no mirror/swap, a no-rotate (or `rotate=180`) value is a likely
  starting point — **[verify]** on the shipping `libepaper.so`.

### 2.4 Environment variables
([qt_epaper](https://developer.remarkable.com/documentation/qt_epaper);
[dragly](https://dragly.org/2017/12/01/developing-for-the-remarkable/))

| Variable | Value(s) | Meaning |
|---|---|---|
| `QT_QPA_PLATFORM` | `epaper` (or `epaper:enable_fonts`) | Select the epaper QPA; `:enable_fonts` enables font support. CLI equiv `-platform epaper`. |
| `QT_QUICK_BACKEND` | `epaper` | Select the epaper QtQuick scenegraph adaptation (`libqsgepaper`) so QtQuick renders into the e-ink path. |
| `QMLSCENE_DEVICE` | `epaper` | Older/equivalent knob for `qmlscene`. |
| `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` | rM1 `rotate=180`; rM2 `rotate=180:invertx`; **PP unpublished** | Stock-Qt evdev-touch params: rotate/invert the touch frame to match the panel. |
| `QT_QPA_GENERIC_PLUGINS` | `evdevtablet` | Load Qt's generic **tablet** plugin so the pen is delivered as `QTabletEvent`. The documented way to get pen alongside the QPA. |

KOReader-specific (only relevant if you run *alongside* xochitl rather than stopping it): `KO_DONT_GRAB_INPUT=1`
(don't `EVIOCGRAB` — §2.5) plus the qtfb-shim set (`LD_PRELOAD=<shim>`, `QTFB_SHIM_INPUT=false`,
`QTFB_SHIM_MODEL=false`, `QTFB_SHIM_MODE=RGB565`). Not part of reMarkable's QPA.
([KOReader #13781](https://github.com/koreader/koreader/issues/13781);
[PR #13620](https://github.com/koreader/koreader/pull/13620))

### 2.5 `struct input_event` size, grabbing, and the QPA conflict
- **`struct input_event` is 24 bytes on Paper Pro (aarch64)** vs 16 bytes on rM2 (armv7l): the 64-bit
  `timeval` widens both fields → on aarch64, `type/code/value` sit at offsets **16/18/20** (rM2: 8/10/12). A
  hand-rolled reader **must** use the 24-byte layout or every field misaligns.
  ([dreeko/remarkable-input-tablet](https://github.com/dreeko/remarkable-input-tablet)) **[PP]**
- **Pen axes/ranges (PP, from `remarkable_mouse` `rmpro` branch — experimental, unverified):** `ABS_X 0–11180`,
  `ABS_Y 0–15340` (these match KOReader's `wacom_width/height`, corroborating), pressure `0–4096` (12-bit),
  distance/hover `0–255`. ([dreeko/remarkable-input-tablet](https://github.com/dreeko/remarkable-input-tablet)) **[PP, partial]**
- **EVIOCGRAB vs share — the central gotcha:**
  - **If you run alongside xochitl:** xochitl already opens/feeds input; grabbing exclusively fights it.
    KOReader's `KO_DONT_GRAB_INPUT=1` exists precisely to *not* take exclusive control. Its PP profile sets
    `canSuspend=no`/`canStandby=no` with the comment *"Suspend and Standby should be handled by xochitl with
    `KO_DONT_GRAB_INPUT=1` set, otherwise bad things will happen."*
    ([device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua);
    [#13781](https://github.com/koreader/koreader/issues/13781))
  - **If you stop xochitl (this project's model):** contention with *xochitl* disappears, but a **second
    conflict is inside your own process** — the epaper/Qt QPA opens the touch node via `QEvdevTouchManager`
    (and the `evdevtablet` plugin opens the pen if enabled). If you *also* open the same node with your own
    reader you get double-delivery, or starve the QPA if you `EVIOCGRAB`. **Decision rule:** *either* (a) let
    the QPA own input → consume touch as QML `MouseArea`/`MultiPointTouchArea` + pen as `QTabletEvent` via
    `evdevtablet`, and open **no** second reader; *or* (b) read evdev yourself **and** stop the QPA grabbing
    the same nodes (don't enable `evdevtablet`; disable/point the QPA's touch device). Don't mix both on one
    node. Qt's stock `QEvdevTouchScreenHandler` does **not** grab by default (only with a `grab=1` spec), so
    the QPA likely *shares* the node — confirm with `fuser /dev/input/eventX` while the app runs. **[verify]**

---

## 3. E-ink refresh control on the Paper Pro

> **Two easily-confused classes:**
> - **`EPFramebuffer`** (lowercase **b**) = **Paper Pro (rMPP)** — color Gallery 3 / ACeP, DRM-backed.
>   Reverse-engineered in [asivery/epfb-re](https://github.com/asivery/epfb-re). **This is the one you want.**
> - **`EPFrameBuffer`** (capital **B**) = older **rM1/rM2** — mono/grayscale, mxcfb-backed.
>   ([canselcik/libremarkable libqsgepaper.md](https://github.com/canselcik/libremarkable/blob/master/reference-material/libqsgepaper.md);
>   [Eeems-Org/remarkable-template-qt-app epframebuffer.h](https://github.com/Eeems-Org/remarkable-template-qt-app/blob/main/src/vendor/epaper/epframebuffer.h))
>
> They are **different classes with different methods and different enums** — never mix them.

The panel is exposed through reMarkable's scenegraph plugin
`/usr/lib/plugins/scenegraph/libqsgepaper.so` (class `QsgEpaperPlugin`, key `epaper`); the platform plugin is
`/usr/lib/plugins/platforms/libepaper.so`. Qt loads plugins `RTLD_LOCAL`, so to call exported symbols you must
`dlopen("…/libqsgepaper.so", RTLD_NOW)` and `dlsym` the **explicit handle** (`RTLD_DEFAULT` won't see them), or
link the epfb-re shim.

### 3.1 Paper Pro `EPFramebuffer` interface — confirmed
From [asivery/epfb-re/epframebuffer.h](https://github.com/asivery/epfb-re/blob/master/epframebuffer.h) **[PP]**:
```cpp
class EPFramebuffer {
public:
    enum UpdateFlag { NoRefresh = 0, CompleteRefresh = 1 };

    // present + refresh; returns a marker (unsigned long)
    unsigned long swapBuffers(QRect rect, EPContentType epct, EPScreenMode mode,
                              QFlags<EPFramebuffer::UpdateFlag> flags);

  #ifdef EPFB_INTERNAL
    static EPFramebuffer *instance();
  #endif
    static EPFramebuffer *createControlledInstance();  // repo's recommended entry point

    QImage *getAuxFramebuffer();    // the back buffer you paint INTO
    QImage *getMainFramebuffer();
};
EPFramebuffer *createEPFramebuffer();
```
Usage model (per the repo): `createControlledInstance()` → `getAuxFramebuffer()` returns the `QImage*` you draw
into → `swapBuffers(...)` swaps + updates the screen. Link `libepfb.so` first; resources auto-clean via
`atexit()`. ([asivery/epfb-re](https://github.com/asivery/epfb-re))

> **2026-08-26 re-check on OS 3.28.0.164:** the on-device `/usr/lib/plugins/scenegraph/libqsgepaper.so` now
> exports `swapBuffers(QRect, EPScreenMode, QFlags<UpdateFlag>)` — **no `EPContentType` arg** (the header above
> matches older 3.28 builds) — plus a multi-region `swapBuffers(QRegion, EPScreenModeMap, QFlags)` and
> `ghostControl(GhostControlMode)`; `instance()` unchanged. Enum values are unchanged.
- **`ghostControl(...)` / `GhostControlMode`, `forceInstance()`, `setBuffers()` are NOT in the epfb-re header** —
  but `ghostControl(GhostControlMode)` **is** exported by the on-device rMPP lib (see the re-check above;
  Eeems reversed `GhostControlMode{ BlinkNow, BlinkLater, BleachNow, FactoryReset }`, `BleachNow`/`FactoryReset`
  ACeP2-only). The proven de-ghost tool remains the periodic full `CompleteRefresh` flash. **[PP]**

### 3.2 rM1/rM2 `EPFrameBuffer` — for contrast, do NOT use on rMPP
- Symbols: `EPFrameBuffer::sendUpdate(QRect, WaveformMode, UpdateMode, bool)`, `clearScreen()`,
  `waitForLastUpdate()`, `instance()`, plus `swapBuffers`, `ghostControl`, `forceInstance`, `setBuffers`,
  `createControlledInstance`, `setForceFull` (the rM2 class *also* has these, but as **capital-B** with mxcfb
  waveform enums). **`Q_INVOKABLE setForceFull(bool)`** is the rM1/rM2 "force a full refresh" knob.
  ([libremarkable libqsgepaper.md](https://github.com/canselcik/libremarkable/blob/master/reference-material/libqsgepaper.md);
  [pl-semiotics/libqsgepaper-snoop](https://github.com/pl-semiotics/libqsgepaper-snoop);
  [Eeems-Org epframebuffer.h](https://github.com/Eeems-Org/remarkable-template-qt-app/blob/main/src/vendor/epaper/epframebuffer.h)) **[rM1/rM2]**
- On **rMPP** the equivalent of `setForceFull(true)` is `swapBuffers(…, Color, QualityFull, CompleteRefresh)`.

### 3.3 Enums and integer values
**Paper Pro — confirmed** ([asivery/epfb-re/epframebuffer.h](https://github.com/asivery/epfb-re/blob/master/epframebuffer.h)) **[PP]**:
```cpp
enum EPScreenMode  { QualityFastest=0, QualityFast=1, Quality3=3, QualityFull=4, Quality5=5 };  // (2 skipped)
enum EPContentType { Mono=0, Color=1 };
enum UpdateFlag    { NoRefresh=0, CompleteRefresh=1 };  // nested in EPFramebuffer
// swapBuffers arg order: (QRect, EPContentType, EPScreenMode, QFlags<UpdateFlag>) on older builds;
// current 3.28 builds (verified 3.28.0.164): (QRect, EPScreenMode, QFlags<UpdateFlag>) — no EPContentType
```

**Independent confirmation via rmBifrost** — [TiagoJMartins/rmBifrost](https://github.com/TiagoJMartins/rmBifrost)
(surviving mirror; original `shg8/rmBifrost` is 404/deleted). Its `refresh_type` maps to the same integer
vocabulary the on-device screen-update function consumes
([global_constants.h](https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/f6e0d575ba8cfc50721ee1b62fee556838ecfa7c/include/bifrost/global_constants.h),
[compositor.cpp](https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/f6e0d575ba8cfc50721ee1b62fee556838ecfa7c/src/compositor/compositor.cpp)) **[PP]**:

| rmBifrost `refresh_type` | EPContentType | EPScreenMode | UpdateFlag | = `swapBuffers(rect, …)` |
|---|---|---|---|---|
| `MONOCHROME` (0) | Mono (0) | QualityFastest (0) | NoRefresh (0) | mono text, fastest |
| `COLOR_ANIMATION` (1) | Color (1) | QualityFastest (0)¹ | NoRefresh (0) | animated color |
| `COLOR_FAST` (2) | Color (1) | QualityFast (1) | NoRefresh (0) | fast color |
| `COLOR_1` (-2) | Color (1) | Quality3 (3) | NoRefresh (0) | color, mid quality |
| `COLOR_CONTENT` (3) | Color (1) | QualityFull (4) | NoRefresh (0) | high-quality color, no flash |
| `COLOR_2` (-3) | Color (1) | Quality5 (5) | NoRefresh (0) | color, highest |
| **`FULL` (4)** | **Color (1)** | **QualityFull (4)** | **CompleteRefresh (1)** | **full color-developing flash** |

¹ rmBifrost's positional triples confirm the **load-bearing** fact regardless of the exact `COLOR_ANIMATION`
quality index: the two reverse-engineering efforts independently produce the same vocabulary — content ∈ {0,1},
quality ∈ {0,1,3,4,5}, flag ∈ {0,1} — and agree that **full color content = `Color` + `QualityFull(4)` +
`CompleteRefresh(1)`**. (rmBifrost reaches the function by LD_PRELOAD-ing into xochitl and inline-hooking
hardcoded addresses; only the *parameter triples* transfer to a clean epfb/QPA approach. Its binary was fw
3.14/3.15-only — reuse the design, not the build.)

### 3.4 Full vs partial vs fast; why color needs FULL; ghosting
- The rMPP "strength" axis is **`EPScreenMode`** (`QualityFastest → QualityFast → Quality3 → QualityFull →
  Quality5`) × **`EPContentType`** (Mono/Color) × the **`CompleteRefresh`** flag.
  ([epfb-re](https://github.com/asivery/epfb-re/blob/master/epframebuffer.h))
- **Color (Gallery 3 / ACeP) content needs a FULL refresh to develop color.** Verified by this project on
  device: the scenegraph auto-refresh handles mono, but **partial/fast waveforms leave color content white or
  show only a fragment** — color needs the full multi-pass waveform. rmBifrost encodes exactly this: its only
  **`FULL`** path sets `CompleteRefresh=1` (the others use `NoRefresh=0`) — i.e. the **`CompleteRefresh` flag
  is what triggers the color-developing flash.**
  (`docs/research-reuse.md`; [rmBifrost compositor.cpp](https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/f6e0d575ba8cfc50721ee1b62fee556838ecfa7c/src/compositor/compositor.cpp)) **[PP]**
  - *Physics [general]:* multi-pigment ACeP drives each particle stack through a long multi-phase waveform; a
    fast/partial waveform can't fully separate pigments → washed-out/undeveloped color until a full waveform
    runs.
- **Color palette / ICC [PP]:** wavexx characterized the rMPP — ~10 discrete pen colors, notably **muted**
  ("white" reads as gray, darker than rM2; backlight shifts black toward blue); publishes an Argyll ICC
  `rmpro-v0.icc` for soft-proof. (Characterizes the *palette*, not waveforms.)
  ([thregr.org/wavexx](https://www.thregr.org/wavexx/rnd/20260201-remarkable_pro_colors/))
- **Ghosting:** `ghostControl(GhostControlMode)` is absent from the public epfb-re header but **is** exported by
  the on-device rMPP `libqsgepaper.so` (verified 2026-08-26 on 3.28.0.164; `GhostControlMode` values reversed by
  Eeems as `BlinkNow/BlinkLater/BleachNow/FactoryReset`). The proven de-ghost tool remains the periodic full flash.
  ([epfb-re — absence](https://github.com/asivery/epfb-re/blob/master/epframebuffer.h);
  [snoop](https://github.com/pl-semiotics/libqsgepaper-snoop)) **[PP]**

### 3.5 Is direct `/dev/dri/card0` DRM panel packing undocumented? — **YES, confirmed**
The 405×1084 transport buffer, the `imx-drm` driver, the RGB-only plane formats, and proprietary
`colortable_*.bin` LUTs are publicly *visible*; the exact **405×1084 → 1620×2160 color/subpixel packing
transform is undocumented and not publicly reverse-engineered** as of 2026-06.
- On-device `drm_info` (real rMPP, [Eeems-Org/remarkable.guide#74](https://github.com/Eeems-Org/remarkable.guide/issues/74)):
  `/dev/dri/card0`, driver **`imx-drm` v1.0.0** (stock NXP, not a custom EPDC), mode **405×1084@84.98**,
  `Subpixel: unknown`, plane advertises only ordinary RGB fourccs (XRGB8888/ARGB8888/RGB565/…) — **no
  e-ink/CFA/packed format.** (`405 × 4 = 1620` = visible width.) Proprietary `colortable_best/fast/pen/std.bin`
  LUTs live in reMarkable userspace. → Transport dims observable; packing transform not public.
- All community projects use libqsgepaper, **not** raw DRM: epfb-re documents only the userspace refresh API;
  rmBifrost LD_PRELOADs xochitl; a rm2fb→rMPP attempt
  ([ddvk/remarkable2-framebuffer#130](https://github.com/ddvk/remarkable2-framebuffer/issues/130)) works at the
  libqsgepaper symbol level and reports it **incomplete** ("found getInstance… clears everything… couldn't get
  the painter working"); goMarkableStream reads the *already-composited* buffer from `/proc/<pid>/mem`, not DRM.
- Docs are stubs: [remarkable.guide display devel](https://remarkable.guide/devel/device/display.html) is an
  explicit FIXME stub; the dev portal documents only the Qt `epaper`/libqsgepaper path. **[PP]**
- *Could-not-fully-verify gap:* reMarkable's kernel `drivers/gpu/drm/` is git-LFS-gated
  ([reMarkable/linux-imx-rm](https://github.com/reMarkable/linux-imx-rm)) so an in-tree packer wasn't 100%
  ruled out; consistent with `imx-drm`/mxsfb the packing is performed downstream in hardware/firmware.

### 3.6 How KOReader / netsurf / Plato schedule refreshes (designs to reuse)
- **KOReader — `FULL_REFRESH_COUNT` + region union** ([frontend/ui/uimanager.lua](https://github.com/koreader/koreader/blob/master/frontend/ui/uimanager.lua)):
  default `DEFAULT_FULL_REFRESH_COUNT = 6` → every 6th partial is promoted to a flashing full (regional →
  `flashui`, full-screen → `full`); enqueued refreshes are unioned via `region:combine()` and upgraded to the
  higher-precedence mode. Refresh ladder: `fast` → `ui` → `flashui`/`full`. Low-level mxcfb backend
  ([ffi/framebuffer_mxcfb.lua](https://github.com/koreader/koreader-base/blob/master/ffi/framebuffer_mxcfb.lua),
  rM1/rM2/generic — *not* rMPP DRM): tracks a wraparound `marker`, `MXCFB_WAIT_FOR_UPDATE_COMPLETE` marker
  waits, dirty-box alignment (`alignment_constraint=8`, MTK color devices use 16). **[rM1/rM2 transport;
  designs reusable]**
- **netsurf-reMarkable — one debounced ~5 Hz thread + MIN/MAX bbox** (logic in
  [libnsfb-reMarkable/src/surface/remarkable/screen.c](https://github.com/alex0809/libnsfb-reMarkable/blob/master/src/surface/remarkable/screen.c)):
  `fb_async_redraw` pthread loops every **200 ms** (`tv_nsec=200000000`); each tick, under `fb_mutex`, iff a
  dirty box accumulated it sends **one** merged update. Coalesces via MIN/MAX over `next_update_{x0,x1,y0,y1}`.
  Fixed params, no adaptivity: always `GC4` waveform, always `UPDATE_MODE_PARTIAL` (no full/de-ghost path), **no
  marker wait, no pixel alignment.** rM1/rM2 only (i.MX6 EPDC, `/dev/fb0`, RGB565) — only the *scheduling
  pattern* transfers to rMPP. **[rM1/rM2 transport; pattern reusable]**
- **Plato** — same generic mxcfb/Kobo-style scheduling family; **no rMPP-specific source found → unverified for
  Paper Pro.** **[rM1/rM2]**

### 3.7 Actionable Phase-4 recommendation (synthesis)
Drive the panel through libqsgepaper/epaper-QPA and reuse rmBifrost's validated triples:

| Event class | `swapBuffers(rect, content, mode, flag)` — older-build arg order; current 3.28 drops `content` (§3.1) |
|---|---|
| Mono text / fast scroll tick | `(Mono, QualityFastest 0, NoRefresh 0)` |
| Fast / animated color | `(Color, QualityFast 1, NoRefresh 0)` |
| Settled color region (post-scroll) | `(Color, Quality3 3 → QualityFull 4, NoRefresh 0)` |
| Page load / navigation, color | `(Color, QualityFull 4, CompleteRefresh 1)` |
| **Periodic anti-ghost / color-develop flush** | `(Color, QualityFull 4, CompleteRefresh 1)` |

Control loop: **coalesce dirty rects → one bounding box**, drained by a single **~200 ms (≈5 Hz) background
thread** (netsurf pattern) → one `swapBuffers` per tick; **promote to a `CompleteRefresh` flash every N
partials** (KOReader `FULL_REFRESH_COUNT≈6`, tune on device) **and on page-load/scroll-end**; **8-px align**
the dirty rect; **inject CSS** (`* { animation:none!important; transition:none!important }` + emulate
`prefers-reduced-motion: reduce`) so CSS doesn't trigger endless partials; only **block on the marker before
the next full**, not every partial. Reference impls to time on-device: epfb-re `test.cpp` /
`OLD/modetest.cpp` (mode-sweep harness), netsurf `screen.c`, KOReader `framebuffer_mxcfb.lua`.

---

## 4. JavaScriptCore / JIT and W^X

> **Bottom line:** **No evidence that reMarkable (incl. Paper Pro) enforces a userspace W^X policy.** The
> verified **`ferrari_defconfig`** has **no SELinux/AppArmor/LSM-MAC, no seccomp-enforcement, no PaX/grsec, no
> userspace `STRICT_*_RWX`**. On stock Linux, RWX JIT mappings are permitted by default → **JSC's JIT is
> expected to work without `JSC_useJIT=0`.** Keep `JSC_useJIT=0` as a *diagnostic/fallback* only.

### 4.1 Does reMarkable block PROT_EXEC|PROT_WRITE / RWX? — evidence says NO
- **Verified kernel config [PP]:** the official **Paper Pro `arch/arm64/configs/ferrari_defconfig`** (extracted
  from the git-LFS kernel tarball, branches `rmpp_6.1.55_*` and `rmpp_6.12.49_*`) contains only
  `CONFIG_SECURITY=y`, `SECURITYFS`, `RANDOMIZE_BASE` (KASLR), `AUDIT`, `TRUSTED/ENCRYPTED_KEYS` — and **zero**
  matches for `SELINUX`, `APPARMOR`, `SMACK`, `TOMOYO`, `YAMA`, `LANDLOCK`, `CONFIG_LSM=`, `SECCOMP`,
  `STRICT_KERNEL_RWX`, `STRICT_MODULE_RWX`, `DEBUG_WX`, `PAX`, `GRSEC`. `CONFIG_SECURITY=y` only enables the LSM
  *framework*; with no MAC module selected, **nothing enforces W^X.**
  ([reMarkable/linux-imx-rm](https://github.com/reMarkable/linux-imx-rm))
  - *Caveat [general]:* a `defconfig` omits arch defaults. On arm64, `SECCOMP` is `def_bool y` (available) but
    imposes nothing unless a process installs a filter; `STRICT_KERNEL_RWX` protects **kernel** text only, not
    userspace JIT pages — irrelevant to JSC.
    ([kspp Recommended_Settings](https://kspp.github.io/Recommended_Settings.html))
- **General Linux [general]:** without an enforcing LSM/PaX, Linux permits RWX `mmap`/`mprotect(PROT_EXEC)`.
  ([mmap(2)](https://man7.org/linux/man-pages/man2/mmap.2.html);
  [mprotect(2)](https://man7.org/linux/man-pages/man2/mprotect.2.html))
- **When W^X *does* break JITs [general, NOT reMarkable]:** it's an LSM policy — SELinux `execmem`/`execheap`/
  `execmod` → the JIT's `mprotect(…,PROT_EXEC)` fails with EACCES. Absent on non-SELinux Linux.
  ([Mozilla bug 506693](https://bugzilla.mozilla.org/show_bug.cgi?id=506693);
  ["A JIT Compiler Skirmish with SELinux"](https://nullprogram.com/blog/2018/11/15/))
- **No reMarkable JIT-failure report found** across r/RemarkableTablet, Eeems, toltec, remarkable.guide, HN.
  (Absence of evidence — but consistent with the clean kernel config.)

→ **Do not assert a reMarkable W^X policy.** Verify empirically before disabling JIT (run a JS-heavy page;
watch for an executable-mmap failure / JIT crash).

### 4.2 JSC JIT control — `JSC_useJIT=0` confirmed
- **JSC options are overridable by env vars of the form `JSC_<option>`** (verbatim from source). So
  **`JSC_useJIT=0`** forces interpreter-only (LLInt, or CLoop if built `ENABLE_C_LOOP`).
  ([WebKit OptionsList.h](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/OptionsList.h);
  [JSC debugging wiki](https://github.com/mnloop/webkit-wiki/blob/master/Debugging-issues-with-JavaScript-execution.md);
  [docs.webkit.org JSC](https://docs.webkit.org/Deep%20Dive/JSC/JavaScriptCore.html))
- Relevant `JSC_*` vars (from `OptionsList.h`): `JSC_useJIT` (master), `JSC_useBaselineJIT`, `JSC_useDFGJIT`
  (default `is64Bit()`), `JSC_useFTLJIT` (64-bit), `JSC_useConcurrentJIT`, **`JSC_jitMemoryReservationSize`**
  (shrink the executable pool — useful on RAM-constrained devices), `JSC_useWasm`; inverse knobs `JSC_useLLInt=0`.
- **Build-time vs runtime [general, WebKit CMake]:** runtime `JSC_useJIT=0` disables JIT but the machinery is
  still compiled in. Build-time **`ENABLE_JIT=OFF` + `ENABLE_C_LOOP=ON`** removes JIT entirely (portable CLoop
  interpreter); they're mutually exclusive (`WEBKIT_OPTION_CONFLICT(ENABLE_JIT ENABLE_C_LOOP)`), and CLoop is
  incompatible with the sampling profiler (also set `ENABLE_SAMPLING_PROFILER=OFF`).
  ([WebKitFeatures.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/WebKitFeatures.cmake);
  precedent: [Buildroot WPE ARMv5/6 patch](http://lists.busybox.net/pipermail/buildroot/2020-October/597431.html))

### 4.3 Why JIT needs RWX, and JSC's fallback — at source level [general]
- JSC reserves one large **fixed executable pool** up front (`FixedVMPoolExecutableAllocator` /
  `initializeJITPageReservation`): ARM64-with-jump-islands **1 GB**, ARM64-without **128 MB**, 32-bit ARM 16 MB
  (overridable by `JSC_jitMemoryReservationSize`). On Linux this is mapped **RWX**
  (`PROT_READ|PROT_WRITE|PROT_EXEC`, `MAP_PRIVATE|MAP_ANON`); `MAP_JIT` is a no-op on Linux (the W^X split is
  Apple-only, gated on `useFastJITPermissions`/APRR).
  ([ExecutableAllocator.cpp](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/jit/ExecutableAllocator.cpp);
  [OSAllocatorPOSIX.cpp](https://github.com/WebKit/WebKit/blob/main/Source/WTF/wtf/posix/OSAllocatorPOSIX.cpp))
- **This RWX `mmap` is exactly what a W^X kernel/LSM would deny** (SELinux `execmem`, PaX `MPROTECT`) →
  EACCES/MAP_FAILED. Since the verified reMarkable kernel has none, it's expected to **succeed.**
- **Fallback:** if the reservation fails, `isValid()` → false and JSC runs the interpreter (it does **not**
  `CRASH()` on a null reservation). `disableJIT()` sets the same state as `JSC_useJIT=0`. (Cf. V8's `--jitless`
  for platforms that forbid executable-memory allocation — iOS/consoles/smart-TVs.)
  ([ExecutableAllocator.cpp](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/jit/ExecutableAllocator.cpp);
  [v8.dev/blog/jitless](https://v8.dev/blog/jitless))

### 4.4 Other engines / locked-down devices [general precedent, not reMarkable]
- **V8/Node:** `--jitless` disables runtime executable-memory allocation (Ignition interpreter; ~40%
  Speedometer regression), specifically for platforms forbidding W^X.
  ([v8.dev/blog/jitless](https://v8.dev/blog/jitless); [nodejs#26758](https://github.com/nodejs/node/issues/26758))
- **SELinux JITs:** Firefox/SpiderMonkey blocked by `execmem` (Mozilla 506693, above).

### 4.5 SELinux / seccomp / WPE bubblewrap sandbox
- **SELinux: confirmed ABSENT** from both ferrari defconfigs. **seccomp:** not in defconfig (arch may
  force-compile it), imposes nothing without an installed filter.
  ([reMarkable/linux-imx-rm](https://github.com/reMarkable/linux-imx-rm);
  [LSM framework](https://docs.kernel.org/admin-guide/LSM/index.html))
- **WPE `ENABLE_BUBBLEWRAP_SANDBOX=OFF` is the correct bring-up choice [general]:** the sandbox isolates
  WebProcess/NetworkProcess via **user namespaces + bubblewrap + xdg-dbus-proxy + libseccomp**, none guaranteed
  on the reMarkable kernel (check `CONFIG_USER_NS` if you ever re-enable it — not seen enabled). It's pure
  security hardening; off does not affect rendering or JIT.
  ([WebKit bug 195169](https://bugs.webkit.org/show_bug.cgi?id=195169);
  [Igalia buildroot sandbox patch](https://patchwork.ozlabs.org/project/buildroot/patch/20191214142216.2609541-1-aperez@igalia.com/);
  [bubblewrap](https://github.com/containers/bubblewrap))
- **mmap/ptrace:** no `YAMA`/`ptrace_scope` in the defconfig; `CONFIG_SECURITY=y` alone restricts neither. No
  relevant restriction for multiprocess WebKit found.

**Recommendation:** build WPE for ferrari with **`ENABLE_JIT=ON`** + **`ENABLE_BUBBLEWRAP_SANDBOX=OFF`**; expect
JIT to work. Document `JSC_useJIT=0` (or rebuild `ENABLE_JIT=OFF -DENABLE_C_LOOP=ON
-DENABLE_SAMPLING_PROFILER=OFF`) as a **fallback** only if on-device testing shows an executable-memory failure.
On a RAM-constrained device, watch the **128 MB / 1 GB** executable pool — `JSC_jitMemoryReservationSize` can
shrink it.

---

## 5. Lifecycle: rootfs, OTA, xochitl, suspend, and crash-reboot (Memfault/watchdog)

### 5.1 Filesystem — what resets, what persists
- **Root `/` is read-only ext4, A/B (two slots).** Temporarily writable via `mount -o remount,rw /` (reverts on
  reboot). Developer mode does **not** disable disk encryption.
  ([Developer Mode](https://developer.remarkable.com/documentation/developer-mode);
  [remarkable.guide FAQ](https://remarkable.guide/faqs.html)) **[PP]**
- **`/etc` and `/usr` live on the A/B rootfs: an OTA (slot swap) rewrites them**, and `/etc` (also
  `/var/lib`, `/srv`) additionally sits under an overlay whose upperdir is tmpfs (`/var/volatile/*`) —
  writes through the overlay vanish on a plain REBOOT; to persist, `remount,rw /` + `umount -R /etc`
  and write the real rootfs beneath (xovi-tripletap's enable.sh pattern). Either way, rootfs customs
  (e.g. units under `/etc/systemd/system/`, `/usr/share/remarkable/suspended.png`) must be re-applied
  after each update. `/tmp` is tmpfs, gone on reboot. **[PP — verified on 3.28.0.164]**
- **Only `/home` survives reboot AND OTA** (on update the whole root partition is replaced with stock OS; only
  `/home` is untouched → root SSH keys / host keys / rootfs customizations must be reapplied after each update).
  `/home` is LUKS-encrypted (`/dev/mapper/home-encrypted-disk`, ~46 GB).
  ([remarkable.guide FAQ](https://remarkable.guide/faqs.html);
  [Toltec guide](https://remarkable.guide/guide/software/toltec.html)) **[PP]**
- **→ Install dev artifacts under `/home/root/rmweb`** (only place that's writable + survives reboot/OTA);
  bundle missing libs + set rpath. Check running slot: `rootdev` (e.g. `/dev/mmcblk2p2`).
  ([remarkable.jms1.info/updates](https://remarkable.jms1.info/info/updates.html)) **[PP]**

### 5.2 OTA — Codex uses SWUpdate (not the old update-engine)
- **PP / Codex (firmware ≥ 3.11.2.5) uses SWUpdate (`.swu`)**, the old `update-engine.service` was removed.
  This device (3.27.x) is firmly swupdate. The on-device unit is literally **`swupdate`** — codexctl runs
  **`systemctl stop swupdate memfaultd`** after installing (which also *confirms both `swupdate` and
  `memfaultd` exist on-device*).
  ([upgrade_engine doc](https://remarkable.guide/devel/device/upgrade_engine.html);
  [Jayy001/codexctl](https://github.com/Jayy001/codexctl)) **[PP]**
- **OTA wipes all rootfs changes; only `/home` survives.** Every OS upgrade **re-enables the "Automatic
  updates" flag** (a one-time disable is undone by the next upgrade).
  ([FAQ](https://remarkable.guide/faqs.html); [jms1 updates](https://remarkable.jms1.info/info/updates.html)) **[PP]**
- **Blocking OTA, by durability:**
  - **Most durable (off-device):** DNS-sinkhole / firewall the update host. (rM2-era host
    `updates.cloud.remarkable.engineering`, endpoint `/service/update2`, configured via `SERVER=` in
    `/usr/share/remarkable/update.conf` — **[verify-for-PP]** whether Codex still uses the same host.)
    ([jms1](https://remarkable.jms1.info/info/updates.html))
  - **On-device, session:** `systemctl stop swupdate` (what codexctl does). Sticky: `systemctl mask swupdate`
    (may not survive reboot/OTA given RO-root + non-persistent overlays). Discover any update timer:
    `systemctl list-timers; systemctl list-units --all | grep -iE 'update|swupdate|memfault'`.
    ([codexctl](https://github.com/Jayy001/codexctl)) **[PP / verify persistence]**
  - *(rM1/rM2 only — does NOT apply: legacy `update-engine.service`, `systemctl disable --now update-engine`.)*

### 5.3 xochitl — let it boot (it unlocks LUKS), then stop it
- **xochitl unlocks the LUKS `/home` at boot** → always **let xochitl start first**, then `systemctl stop
  xochitl`; restore with `systemctl start xochitl` on exit (trap EXIT). **Never disable xochitl pre-unlock** or
  `/home` (your install dir) won't be mounted. `rm-sync.service` follows xochitl. PIN/lock lives inside
  xochitl, so with it stopped + your app foreground the PIN doesn't appear.
  (`docs/research-reuse.md`; general xochitl role: [remarkable.guide](https://remarkable.guide)) **[PP]**
```sh
systemctl stop xochitl            # /home already unlocked because xochitl booted
QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper /home/root/rmweb/app
systemctl start xochitl           # restore on exit
```

### 5.4 Power / sleep / suspend / frontlight
- **The idle/auto-sleep timer is owned by xochitl** (`IdleSuspendDelay` ms in
  `~/.config/remarkable/xochitl.conf`, default ~20 min). **With xochitl stopped, nothing auto-suspends — your
  foreground app owns idle→sleep.** (KOReader's PP profile sets `canSuspend=no`, noting suspend is normally
  "handled by xochitl.")
  ([ddvk/remarkable-hacks#186](https://github.com/ddvk/remarkable-hacks/issues/186);
  [KOReader device.lua](https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua)) **[PP]**
- **Suspend programmatically:** `systemctl suspend` (canonical; logind → `echo mem > /sys/power/state`). RTC
  wake: `echo <epoch> > /sys/class/rtc/rtc0/wakealarm` (write `0` first to clear; **[verify]** the rtc index —
  i.MX8M has SNVS RTC, may expose two: `cat /sys/class/rtc/rtc0/name`).
  ([KOReader](https://github.com/koreader/koreader); [kernel sleep-states](https://docs.kernel.org/admin-guide/pm/sleep-states.html))
- **Suspend drops USB networking [general / verify-PP]:** S3 powers down the USB gadget controller → host sees
  a disconnect and **SSH over `10.11.99.1` dies while suspended**, recovering on wake. Verify: `ping
  10.11.99.1` from a second session during `systemctl suspend`.
  ([USB persist](https://www.kernel.org/doc/html/v4.13/driver-api/usb/persist.html))
- **Pre-suspend "sleep screen" hook — it's logind `PrepareForSleep`, NOT a sysfs-PID/RT-signal driver.** An
  exhaustive search found **no `target_pid` sysfs node and no SIGRTMAX-1/SIGRTMAX suspend mechanism** on
  reMarkable (that hypothesis is unsupported by any public source). The real hook is the standard
  **systemd-logind `org.freedesktop.login1` → `Manager.PrepareForSleep(bool)` D-Bus signal + a delay inhibitor
  lock.** Oxide (full PP support; maps `"reMarkable Ferrari"` → `RMPP`) takes a delay-inhibitor and on
  `PrepareForSleep(true)` loads `/usr/share/remarkable/sleeping.png`, full-refreshes the e-ink, then releases.
  ([Eeems-Org/oxide](https://github.com/Eeems-Org/oxide);
  [systemd inhibitor locks](https://systemd.io/INHIBITOR_LOCKS/);
  [login1 D-Bus](https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html)) **[PP]**
  - **For your app:** either (a) take a logind delay-inhibitor + listen for `PrepareForSleep(true)` (sd-bus /
    Qt DBus), paint with a full epaper refresh, release; **or (b)** since you call `systemctl suspend`
    yourself, just paint the sleep image immediately before that call. To check what xochitl does empirically:
    `busctl monitor org.freedesktop.login1` then press power.
- **Frontlight (PP-specific — rM2 has none):** `/sys/class/backlight/rm_frontlight/brightness`, **range
  0–2047** (0=off); siblings `max_brightness`, `linear_mapping` (`echo yes > …/linear_mapping`).
  ([remarkable.guide display](https://remarkable.guide/devel/device/display.html);
  [KOReader](https://github.com/koreader/koreader);
  [unreMarkableLabs/reLuminate](https://github.com/unreMarkableLabs/reLuminate)) **[PP]**
- **Sleep/power images:** `/usr/share/remarkable/{suspended,sleeping,poweroff}.png`; rMPP native panel image =
  2160×2880. ([remarkable.guide screens](https://remarkable.guide/guide/config/screens.html);
  [oxide](https://github.com/Eeems-Org/oxide)) **[PP]**

### 5.5 The observed "segfault → device reboots" — it's **systemd's xochitl start-limit**, not Memfault (and not the hw watchdog)
**reMarkable ships Memfault on the Paper Pro** (reMarkable's own case study; codexctl stops `memfaultd`).
([Memfault case study](https://memfault.com/customers/remarkable-case-study/);
[codexctl](https://github.com/Jayy001/codexctl)) **[PP]**

**But Memfault does NOT reboot on crash:**
- **`memfault-core-handler` captures the core and exits — no `reboot()`/reset call** (every path ends `Ok/Err`).
  ([memfault-linux-sdk memfault_core_handler/mod.rs](https://github.com/memfault/memfault-linux-sdk)) **[general]**
- **`memfaultd` reboot-reason tracking is passive** — it only *classifies* a reboot that already happened
  (pstore/ramoops, `last_reboot_reason_file`, systemd shutdown state) and uploads it; it never initiates
  reboots. (`memfaultctl reboot` is an explicit operator command.)
  ([reboot-reason-tracking](https://docs.memfault.com/docs/linux/reboot-reason-tracking);
  [memfaultd config](https://docs.memfault.com/docs/linux/reference-memfaultd-configuration)) **[general]**
- Likely `core_pattern` when memfaultd is active (a pipe, not a reboot):
  `|/usr/sbin/memfault-core-handler -c /etc/memfaultd.conf %P %e %I %s` — confirm with `cat
  /proc/sys/kernel/core_pattern`. ([coredumps](https://docs.memfault.com/docs/linux/coredumps)) **[general / verify]**

**The real culprit = a systemd per-unit failure limit (verified — hypothesis #1 below). The hardware watchdog is
also armed on the Paper Pro (relevant background):**
- **The i.MX8M Mini hardware watchdog is enabled in reMarkable's kernel [PP].** `ferrari.dtsi` declares
  `&wdog1 { …, fsl,ext-reset-output; status = "okay"; }` (routes a timeout to the SoC external-reset pin
  `WDOG_B` = a true hardware reboot); defconfig sets `CONFIG_WATCHDOG=y` + `CONFIG_IMX2_WDT=y` (driver
  `imx2-wdt`, default 60 s) → **`/dev/watchdog0` exists**. `CONFIG_WATCHDOG_SYSFS` is **not** set, so
  `/sys/class/watchdog/…` files may be absent.
  ([reMarkable/linux-imx-rm](https://github.com/reMarkable/linux-imx-rm);
  [imx2_wdt.c](https://github.com/torvalds/linux/blob/master/drivers/watchdog/imx2_wdt.c))
- **A plain SIGSEGV doesn't by itself stop the watchdog kick.** Most plausible explanations [general / verify-PP], ranked:
  1. **A systemd unit reacting to the supervised process's death** — if the crashing process (or a supervisor
     like xochitl / a reMarkable unit) has `WatchdogSec=` with `Restart=on-watchdog` / `StartLimitAction=
     reboot|reboot-force`, losing `WATCHDOG=1` → restart storm hits the start-limit → **systemd reboots.**
     ([systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html))
     **[PP — verified 2026-08-26, OS 3.28.0.164]: this is the mechanism, and the unit is `xochitl` itself — no
     watchdog involved.** Stock `/usr/lib/systemd/system/xochitl.service.d/xochitl-service-override.conf`:
     `Restart=on-failure`, `RestartMode=direct`, `StartLimitIntervalSec=600`, `StartLimitBurst=4`,
     `OnFailure=emergency.target` → `rm-emergency.sh` → **reboot**. The burst counter counts **every** start
     attempt, so a few xochitl restarts within 10 min (xovi toggle + app launch/quit cycles) emergency-reboot
     with no actual crash. The rmweb launcher (`device/rmweb`) mitigates: waits for rmweb-wpeqt to die, then
     `systemctl reset-failed xochitl` before `systemctl start xochitl`.
  2. **The crashing process (or PID 1 with non-zero `RuntimeWatchdogSec`) was petting `/dev/watchdog0`** and the
     crash cascaded into a hang → the unfed **hardware watchdog** fires `WDOG_B`. (Also: closing `/dev/watchdog`
     without the magic `V` leaves the timer running.)
     ([watchdog-api](https://www.kernel.org/doc/html/latest/watchdog/watchdog-api.html);
     [systemd-system.conf](https://www.freedesktop.org/software/systemd/man/latest/systemd-system.conf.html))
  3. **NOT** coredump-capture timeout — the handler just exits; no reboot path.

**Diagnose on-device (BusyBox), `journalctl -b -1` is the deciding command:**
```sh
journalctl -b -1 -n 200          # last-boot log: Watchdog / start-limit-hit / reboot-force / Oops / panic
                                  # "Failed to look up boot -1" => journald not persistent (no evidence kept)
systemctl status memfaultd; cat /etc/memfaultd.conf
cat /proc/sys/kernel/core_pattern                 # is it |/usr/sbin/memfault-core-handler … ?
ls -l /dev/watchdog*                              # expect /dev/watchdog0
systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec   # 0 => systemd NOT petting the hw wdog
dmesg | grep -iE 'watchdog|wdog|imx2|reset'
fuser /dev/watchdog0 2>/dev/null                  # who holds the watchdog open
cat /sys/class/watchdog/watchdog0/bootstatus 2>/dev/null  # reset bit => last boot WAS the watchdog
```

**Disable / mitigate for development:**
```sh
# 1) Stop Memfault capturing/uploading your crashes (also frees core_pattern):
memfaultctl disable-data-collection        # official off-switch (re-enable: enable-data-collection)
systemctl stop memfaultd                    # what codexctl does;  systemctl mask memfaultd  = sticky
# 2) Get a real core instead of the memfault pipe (do this AFTER stopping memfaultd, which rewrites core_pattern on start):
mkdir -p /home/root/cores
echo '/home/root/cores/core.%e.%p' > /proc/sys/kernel/core_pattern
ulimit -c unlimited
# 3) ONLY if diagnostics show RuntimeWatchdogUSec != 0 — disable systemd hw-watchdog petting:
#    [Manager] RuntimeWatchdogSec=0  in /etc/systemd/system.conf (drop-in)  then  systemctl daemon-reexec
#    (If a specific UNIT is the culprit, clear its WatchdogSec=/StartLimitAction= in a drop-in instead.)
```
- `memfaultctl disable-data-collection` is the documented off-switch
  ([memfaultctl CLI](https://docs.memfault.com/docs/linux/reference-memfaultctl-cli)); memfaultd **rewrites
  `core_pattern` on (re)start**, so stop/mask it before setting your own
  ([coredumps](https://docs.memfault.com/docs/linux/coredumps)); `RuntimeWatchdogSec=0` ⇒ "no watchdog device
  is opened, configured, or pinged"
  ([systemd-system.conf](https://www.freedesktop.org/software/systemd/man/latest/systemd-system.conf.html)).
- **Persistence caveat [PP]:** `/etc` edits + `mask` symlinks written through the overlay land on tmpfs
  (`/var/volatile/etc`) and vanish on reboot — write the rootfs beneath the overlay instead
  (`remount,rw /` + `umount -R /etc`); even then an OTA (slot swap) rewrites `/etc`/`/usr`.
  ([FAQ](https://remarkable.guide/faqs.html))

**Net:** Memfault is present but does not reboot on crash. The segfault→reboot is the **systemd start-limit
path** — verified 2026-08-26 on 3.28.0.164: stock `xochitl.service` hits `StartLimitBurst=4` within
`StartLimitIntervalSec=600` (every start attempt counts, crash or not) → `OnFailure=emergency.target` →
`rm-emergency.sh` → reboot. The **i.MX8MM hardware watchdog** (`/dev/watchdog0`, enabled in `ferrari.dtsi`) is
armed but is not the culprit. For dev, `memfaultctl disable-data-collection` + `systemctl stop swupdate
memfaultd`, keep xochitl restarts few, and `systemctl reset-failed xochitl` before any manual start.

---

## 6. Corrections to prior project assumptions

Items where this research **contradicts or refines** earlier notes in `docs/device-profile.md` /
`docs/research-reuse.md` — flagged for an on-device check before relying on them:

1. **Input node mapping is in conflict — verify by name, not `eventN`.** `device-profile.md` (on-device recon,
   2026-06-24) has **event2 = Elan *touch*, event3 = Elan *marker* (pen)**; KOReader `device.lua` (the port's
   source-of-record) has **event2 = *pen*, event3 = *touch*** — the opposite. Both can't be right for the same
   firmware; node numbering also shifts on mainline-kernel builds. **Resolve with `cat /proc/bus/input/devices`
   and bind by device name / `by-path`** (§2.2). This is load-bearing for any direct evdev reader.
2. **No reMarkable W^X policy → don't pre-emptively disable JIT.** The verified `ferrari_defconfig` has no
   SELinux/LSM/PaX. Build `ENABLE_JIT=ON`; treat `JSC_useJIT=0` as a fallback only (§4). (Earlier framing
   implied W^X might force interpreter-only — evidence says JIT should work.)
3. **The crash-reboot is systemd's xochitl start-limit, not `memfault-core-handler`.** Memfault captures +
   exits; the reboot is stock `xochitl.service`'s `StartLimitIntervalSec=600`/`StartLimitBurst=4` +
   `OnFailure=emergency.target` (→ `rm-emergency.sh` → reboot) tripping on repeated restarts — verified
   2026-08-26 on 3.28.0.164 (§5.5). The hardware `/dev/watchdog0` (`ferrari.dtsi` `fsl,ext-reset-output`) is
   armed but is not the culprit.
4. **Pre-suspend hook = logind `PrepareForSleep` D-Bus, not a `target_pid` sysfs + SIGRTMAX signal driver.**
   The signal-driver hypothesis in `research-reuse.md §4` is unsupported by any public source; use a logind
   delay-inhibitor or paint before `systemctl suspend` (§5.4).
5. **`EPFramebuffer::ghostControl` / `GhostControlMode` is not in the public epfb-re header but IS exported by
   the on-device rMPP lib** (verified 2026-08-26 on 3.28.0.164; values reversed by Eeems:
   `BlinkNow/BlinkLater/BleachNow/FactoryReset`). The proven de-ghost path is still the periodic full
   `CompleteRefresh` flash (§3.1/§3.4).
6. **OTA engine is SWUpdate (`swupdate` unit), not `update-engine`** on this Codex firmware — block via
   `systemctl stop swupdate` (or DNS), not the rM2-era `update-engine.service` (§5.2).

---

## 7. Source index

**Official reMarkable**
- Qt epaper guide: https://developer.remarkable.com/documentation/qt_epaper
- Developer Mode: https://developer.remarkable.com/documentation/developer-mode
- Read-on-reMarkable extension: https://support.remarkable.com/s/article/Read-on-reMarkable-Google-Chrome-Extension
- Kernel fork (ferrari.dtsi + ferrari_defconfig, LFS-gated): https://github.com/reMarkable/linux-imx-rm

**Community wiki / guides**
- remarkable.guide: FAQ https://remarkable.guide/faqs.html · display (stub) https://remarkable.guide/devel/device/display.html · upgrade engine https://remarkable.guide/devel/device/upgrade_engine.html · screens https://remarkable.guide/guide/config/screens.html · Toltec https://remarkable.guide/guide/software/toltec.html
- jms1 updates: https://remarkable.jms1.info/info/updates.html
- Toltec discussion (rMPP differences): https://github.com/toltec-dev/toltec/discussions/910 · https://toltec-dev.org/stable/
- awesome-reMarkable: https://github.com/reHackable/awesome-reMarkable
- on-device drm_info (rMPP): https://github.com/Eeems-Org/remarkable.guide/issues/74

**Browsers / readers**
- NetSurf: https://github.com/alex0809/netsurf-reMarkable · https://github.com/alex0809/netsurf-base-reMarkable · https://github.com/alex0809/libnsfb-reMarkable · https://akselmo.dev/posts/netsurf-on-remarkable-2/
- KOReader: https://github.com/koreader/koreader · PR #13620 https://github.com/koreader/koreader/pull/13620 · issue #12856 https://github.com/koreader/koreader/issues/12856 · device.lua https://github.com/koreader/koreader/blob/master/frontend/device/remarkable/device.lua · koreader-base framebuffer_mxcfb.lua https://github.com/koreader/koreader-base/blob/master/ffi/framebuffer_mxcfb.lua · uimanager.lua https://github.com/koreader/koreader/blob/master/frontend/ui/uimanager.lua
- Plato: https://github.com/LinusCDE/plato

**epaper QPA / input**
- Open QPA: https://github.com/reMarkable/epaper-qpa · https://github.com/reMarkable/qt5-qpa-epaper · integration https://github.com/reMarkable/epaper-qpa/blob/master/epaperintegration.cpp
- dragly dev guide (env vars, evdevtablet): https://dragly.org/2017/12/01/developing-for-the-remarkable/
- input/struct sizes/pen ranges: https://github.com/dreeko/remarkable-input-tablet
- rM2 Wacom digitizer: https://github.com/canselcik/libremarkable/wiki/Reading-from-Wacom-I2C-Digitizer

**E-ink refresh / EPFramebuffer**
- asivery/epfb-re (rMPP header + enums): https://github.com/asivery/epfb-re · header https://github.com/asivery/epfb-re/blob/master/epframebuffer.h
- rmBifrost (triples): https://github.com/TiagoJMartins/rmBifrost · global_constants.h / compositor.cpp (pinned) https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/f6e0d575ba8cfc50721ee1b62fee556838ecfa7c/include/bifrost/global_constants.h · https://raw.githubusercontent.com/TiagoJMartins/rmBifrost/f6e0d575ba8cfc50721ee1b62fee556838ecfa7c/src/compositor/compositor.cpp
- rM1/rM2 EPFrameBuffer: https://github.com/canselcik/libremarkable/blob/master/reference-material/libqsgepaper.md · https://github.com/pl-semiotics/libqsgepaper-snoop · https://github.com/Eeems-Org/remarkable-template-qt-app/blob/main/src/vendor/epaper/epframebuffer.h
- netsurf screen.c: https://github.com/alex0809/libnsfb-reMarkable/blob/master/src/surface/remarkable/screen.c
- color/ICC: https://www.thregr.org/wavexx/rnd/20260201-remarkable_pro_colors/
- rm2fb→rMPP attempt: https://github.com/ddvk/remarkable2-framebuffer/issues/130

**JIT / W^X / WebKit**
- WebKit: OptionsList.h https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/OptionsList.h · ExecutableAllocator.cpp https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/jit/ExecutableAllocator.cpp · OSAllocatorPOSIX.cpp https://github.com/WebKit/WebKit/blob/main/Source/WTF/wtf/posix/OSAllocatorPOSIX.cpp · WebKitFeatures.cmake https://github.com/WebKit/WebKit/blob/main/Source/cmake/WebKitFeatures.cmake · JSC docs https://docs.webkit.org/Deep%20Dive/JSC/JavaScriptCore.html · debugging wiki https://github.com/mnloop/webkit-wiki/blob/master/Debugging-issues-with-JavaScript-execution.md
- WPE sandbox: https://bugs.webkit.org/show_bug.cgi?id=195169 · https://patchwork.ozlabs.org/project/buildroot/patch/20191214142216.2609541-1-aperez@igalia.com/ · https://github.com/containers/bubblewrap
- V8 jitless: https://v8.dev/blog/jitless · https://github.com/nodejs/node/issues/26758
- SELinux JIT: https://bugzilla.mozilla.org/show_bug.cgi?id=506693 · https://nullprogram.com/blog/2018/11/15/
- mmap/mprotect: https://man7.org/linux/man-pages/man2/mmap.2.html · https://man7.org/linux/man-pages/man2/mprotect.2.html · LSM https://docs.kernel.org/admin-guide/LSM/index.html · kspp https://kspp.github.io/Recommended_Settings.html

**Lifecycle / Memfault / watchdog / suspend**
- Memfault: case study https://memfault.com/customers/remarkable-case-study/ · coredumps https://docs.memfault.com/docs/linux/coredumps · reboot-reason https://docs.memfault.com/docs/linux/reboot-reason-tracking · memfaultd config https://docs.memfault.com/docs/linux/reference-memfaultd-configuration · memfaultctl https://docs.memfault.com/docs/linux/reference-memfaultctl-cli · linux-sdk https://github.com/memfault/memfault-linux-sdk
- codexctl (`systemctl stop swupdate memfaultd`): https://github.com/Jayy001/codexctl
- Oxide (PrepareForSleep, Ferrari→RMPP, sleep images): https://github.com/Eeems-Org/oxide
- watchdog: https://www.kernel.org/doc/html/latest/watchdog/watchdog-api.html · https://github.com/torvalds/linux/blob/master/drivers/watchdog/imx2_wdt.c
- systemd: https://www.freedesktop.org/software/systemd/man/latest/systemd-system.conf.html · https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html · https://systemd.io/INHIBITOR_LOCKS/ · https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html
- frontlight: https://github.com/unreMarkableLabs/reLuminate
- suspend/USB/RTC: https://docs.kernel.org/admin-guide/pm/sleep-states.html · https://www.kernel.org/doc/html/v4.13/driver-api/usb/persist.html
- xochitl idle: https://github.com/ddvk/remarkable-hacks/issues/186

**Ecosystem (app-stack prior art)**
- XOVI / qtfb / shim / appload: https://github.com/asivery/xovi · https://github.com/asivery/qtfb · https://github.com/asivery/rmpp-qtfb-shim · https://github.com/asivery/rm-appload
- goMarkableStream (reads composited buffer via /proc/pid/mem): https://github.com/owulveryck/goMarkableStream

> Project-internal corroborating docs (not URLs): `docs/device-profile.md`, `docs/research-reuse.md`.
