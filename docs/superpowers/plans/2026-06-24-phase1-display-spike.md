# Phase 1 — Display Spike Implementation Plan

> **For agentic workers:** implement task-by-task; each task ends with a concrete verification gate. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Prove that arbitrary pixels from *our* cross-compiled binary reach the Paper Pro e-ink screen, via Qt6 + the official `epaper` QPA, with `xochitl` stopped and reliably restored.

**Architecture:** A minimal **QtGui `QRasterWindow`** paints a test pattern (grayscale bars, frame, diagonals, dimensions text). It runs fullscreen with `QT_QPA_PLATFORM=epaper` while `xochitl` is stopped; the on-device `libepaper.so` plugin does the panel packing + e-ink refresh for us. This is the display path from the spec (option A).

**Tech Stack:** Qt6 (Gui only — Widgets is absent on device), CMake+Ninja via the ferrari SDK, evidence-by-eyeball on the e-ink panel.

## Global Constraints

- QtGui only — **no QtWidgets on the device** (use `QRasterWindow`, not widgets).
- Cross-compile via SDK: `cmake` auto-uses `$CMAKE_TOOLCHAIN_FILE=/opt/rmpp-sdk/.../OEToolchainConfig.cmake` after sourcing the env (wires Qt6 cross + `-mcpu=cortex-a53+crc+crypto`).
- Run with `QT_QPA_PLATFORM=epaper`; the plugin lives on-device at `/usr/lib/plugins/platforms/libepaper.so`.
- **Always restore xochitl** (trap on EXIT) — never leave the device UI down.
- Install/run under `/home/root/rmweb/` only.

## File Structure

- `display/spike/main.cpp` — `QRasterWindow` test pattern; auto-quits after N seconds.
- `display/spike/CMakeLists.txt` — links `Qt6::Gui`.
- `scripts/cmake-build.sh` — configure+build a CMake project in the SDK container.
- `scripts/run-on-device.sh` — deploy, stop xochitl, run via epaper, ALWAYS restart xochitl.

---

### Task 1: CMake build helper

**Files:** Create `scripts/cmake-build.sh`.
**Interfaces:** Produces `scripts/cmake-build.sh <src-dir> [name]` → builds into `build/<name>/`; reused by all later C++/Qt/WPE phases.

- [ ] Step 1: Write `scripts/cmake-build.sh` (cmake -S/-B -G Ninja, SDK env sourced).
- [ ] Step 2: `chmod +x`. (Verified together with Task 2's build.)

---

### Task 2: QRasterWindow test app

**Files:** Create `display/spike/main.cpp`, `display/spike/CMakeLists.txt`.
**Interfaces:** Produces `build/spike/display-spike` (aarch64 ELF linking `libQt6Gui`).

- [ ] Step 1: Write `main.cpp` (QGuiApplication + QRasterWindow paints test pattern; `argv[1]` = seconds before auto-quit, default 12).
- [ ] Step 2: Write `CMakeLists.txt` (`find_package(Qt6 COMPONENTS Gui)`, link `Qt6::Gui`).
- [ ] Step 3: Build: `scripts/cmake-build.sh display/spike spike`. Expected: builds cleanly.
- [ ] Step 4: Verify: `file build/spike/display-spike` → `ELF 64-bit … ARM aarch64 … dynamically linked`. Optionally confirm it links Qt6Gui.

---

### Task 3: Run on device via epaper (the visual proof)

**Files:** Create `scripts/run-on-device.sh`.
**Interfaces:** Produces `scripts/run-on-device.sh <binary> [args]` — stop xochitl → run via epaper → restore xochitl; reused by later phases.

- [ ] Step 1: Write `scripts/run-on-device.sh` (scp; ssh: `systemctl stop xochitl`; `trap … systemctl start xochitl` on EXIT; `QT_QPA_PLATFORM=epaper <remote> <args>`).
- [ ] Step 2: `chmod +x`.
- [ ] Step 3: Run: `scripts/run-on-device.sh build/spike/display-spike 12`. **User watches the device.**
- [ ] Step 4 (verification gate): The e-ink panel shows the test pattern (grayscale bars + frame + diagonals + "rmweb display spike / WxH"). After ~12 s, xochitl returns. User confirms.

**Fallback:** if the `epaper` QPA does not present `QRasterWindow` content (xochitl only exercises the QtQuick path), pivot to a minimal QML app rendered via the `libqsgepaper` scenegraph plugin (present in the sysroot). Same run/restore script.

## Self-Review
- Covers spec Phase 1 (display spike via epaper QPA, xochitl stopped). ✓
- No placeholders; exact commands/flags. The one unknown (raster vs QML under epaper) has an explicit fallback. ✓
- Reuses `scripts/` helpers consistently; `run-on-device.sh` and `cmake-build.sh` carry forward to later phases. ✓
