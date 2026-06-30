# rmweb Phase 5 — Packaging & Launch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn rmweb into a self-contained installable app under `/home/root/rmweb` that launches on the device, always restores xochitl on any exit, survives reboot/OTA, and exposes a tappable icon via rm-appload.

**Architecture:** A hardened POSIX-sh on-device launcher (`device/rmweb`) sources one shared env file (`device/rmweb-env.sh`) and runs the existing `rmweb-wpeqt` binary, with a `trap` that restores xochitl on every exit path. An idempotent `device/install.sh` wires it up and (when present) registers an rm-appload icon. The app gains a `⏻` chrome button that calls `std::_Exit(0)` to quit cleanly back to the launcher. `scripts/bundle.sh` ships the new files; the dev runner is refactored to reuse the shared env. No-brick logic is unit-tested on the host with stubbed `systemctl`/`mount`.

**Tech Stack:** POSIX shell (BusyBox-compatible), C++17/Qt6 (`engine/wpeqt/main.cpp`), bash host test harness, clang++ for existing C++ host tests.

## Global Constraints

- Install **only** under `/home/root/rmweb`. Touch nothing in `/etc` or rootfs (rootfs is full/read-only; only `/home` survives OTA).
- Device is **BusyBox**: no `timeout`, no `flock`, no `pkill`. Use `mkdir` for atomic locks, `kill -0` loops, `pgrep`+`kill` for subprocess teardown, `head -n N`.
- **Never `disable` xochitl** — only `stop`/`start`. xochitl must run at boot to unlock the LUKS `/home` partition.
- Launcher shell is `#!/bin/sh`, POSIX-only (no `local`, no bashisms) so it runs under BusyBox ash on-device and `sh` on the host test.
- App quit path uses `std::_Exit(0)` (no `exit(0)`/`return` from `main`) to skip WebKit static-destructor `SIGABRT` (the `webkit-clean-exit-abort-save-mode` finding). `_Exit(0)` = code 0, no signal → no watchdog reboot.
- The production env is the source of truth in `device/rmweb-env.sh`; lifted verbatim from `scripts/run-wpeqt-on-device.sh` (the proven `show` path). Keep the `RMWEB_JIT`/`RMWEB_JSC_OPTS`/`RMWEB_UA`/`RMWEB_BLOCK`/`RMWEB_SITECSS`/`RMWEB_DPR`/`RMWEB_READER_FONT`/`RMWEB_READER_DIR`/`RMWEB_FULL_EVERY`/`RMWEB_PRESENT_DWELL` levers.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- **Every commit guards `.env`:** `if git check-ignore -q .env; then git add <specific files>; git commit ...; else echo ABORT; fi`. Never `git add -A`/`.`.

## File Structure

| File | Responsibility |
|------|----------------|
| `device/rmweb-env.sh` (new) | Single source of truth for the production runtime env; sourced by the launcher and the dev runner. |
| `device/rmweb` (new) | On-device production launcher: lock → stop xochitl → overlay-mount → source env → run app → `trap` restores xochitl on any exit. |
| `device/install.sh` (new) | Idempotent on-device installer / OTA re-hook: integrity check, chmod, version stamp, (layer B) rm-appload registration. |
| `device/appload/rmweb.draft` (new) | rm-appload descriptor (layer B; format verified on-device). |
| `device/icon.svg` (new) | App icon source (layer B). |
| `tests/launcher_test.sh` (new) | Host no-brick tests for `device/rmweb` with stubbed `systemctl`/`mount`. |
| `tests/install_test.sh` (new) | Host tests for `device/install.sh` integrity/chmod/version. |
| `engine/wpeqt/main.cpp` (modify) | Add the `⏻` exit button: `Hit::Power`, `kPowerW`, `iconPower`, layout shift, tap-router case. |
| `scripts/bundle.sh` (modify) | Ship the launcher, env, installer, VERSION, icon, appload descriptor in the bundle. |
| `scripts/run-tests.sh` (modify) | Also run `tests/*_test.sh` and (optional) shellcheck the shell files. |
| `scripts/run-wpeqt-on-device.sh` (modify) | Source `rmweb-env.sh` instead of inlining env (DRY). |
| `docs/install.md` (new) | User/dev install + run + OTA-recovery docs. |

---

### Task 1: Production launcher + shared env + host no-brick tests

**Files:**
- Create: `device/rmweb-env.sh`, `device/rmweb`, `tests/launcher_test.sh`
- Modify: `scripts/run-tests.sh`

**Interfaces:**
- Produces: `device/rmweb` — invoked as `RMWEB_ROOT=<dir> sh device/rmweb [URL]`; honours env `RMWEB_ROOT` (default `/home/root/rmweb`); exit 1 + no xochitl change if `$RMWEB_ROOT/.lock` already exists.
- Produces: `device/rmweb-env.sh` — sourced fragment; expects `$R` (or `$RMWEB_ROOT`) set; exports the production env.
- Consumes: `$RMWEB_ROOT/bin/rmweb-wpeqt` (the app), `$RMWEB_ROOT/libexec/wpe-webkit-2.0/` (WPE helpers).

- [ ] **Step 1: Write the failing test** — `tests/launcher_test.sh`

```sh
#!/usr/bin/env bash
# Host no-brick tests for device/rmweb. Stubs systemctl/mount/umount/pgrep on PATH so nothing real is
# touched; asserts xochitl is ALWAYS restored (start) after a stop, on clean exit / crash / TERM, and is
# NOT restored when we never stopped it or when another instance holds the lock.
set -u
ROOT_REPO="$(cd "$(dirname "$0")/.." && pwd)"
LAUNCHER="$ROOT_REPO/device/rmweb"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0

make_stub(){ # $1=name  $2=body
  mkdir -p "$STUBS"; { echo '#!/bin/sh'; echo "$2"; } > "$STUBS/$1"; chmod +x "$STUBS/$1"; }

setup(){ # fresh device-root + stubs + fake app; $1 = app mode (clean|crash|hang)
  R="$TMP/r.$RANDOM"; STUBS="$R/stubs"; STUBLOG="$R/stub.log"
  mkdir -p "$R/bin" "$R/libexec/wpe-webkit-2.0" "$STUBS"; : > "$STUBLOG"
  cp "$ROOT_REPO/device/rmweb-env.sh" "$R/rmweb-env.sh"
  make_stub systemctl 'echo "systemctl $*" >> "'"$STUBLOG"'"; [ "$1" = is-active ] && { [ "${XOCHITL_ACTIVE:-1}" = 1 ] && exit 0 || exit 3; }; exit 0'
  make_stub mount  'echo "mount $*" >> "'"$STUBLOG"'"; exit 0'
  make_stub umount 'echo "umount $*" >> "'"$STUBLOG"'"; exit 0'
  make_stub pgrep  'exit 1'
  { echo '#!/bin/sh'; echo 'echo "app $*" >> "'"$STUBLOG"'"';
    echo 'case "${APP_MODE:-clean}" in clean) exit 0;; crash) exit 1;; hang) sleep 30;; esac'; } > "$R/bin/rmweb-wpeqt"
  chmod +x "$R/bin/rmweb-wpeqt"
}

want(){   grep -q "$1" "$STUBLOG" || { echo "  FAIL: expected '$1'"; cat "$STUBLOG"; fails=$((fails+1)); }; }
nowant(){ grep -q "$1" "$STUBLOG" && { echo "  FAIL: unexpected '$1'"; cat "$STUBLOG"; fails=$((fails+1)); } || true; }

echo "case 1: clean exit restores xochitl"
setup; APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER" about:blank
want "systemctl stop xochitl"; want "systemctl start xochitl"; want "mount -t overlay"; want "umount /usr/libexec"
[ -d "$R/.lock" ] && { echo "  FAIL: lock not released"; fails=$((fails+1)); }

echo "case 2: app crash still restores xochitl"
setup; APP_MODE=crash PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER"
want "systemctl stop xochitl"; want "systemctl start xochitl"

echo "case 3: do not restart xochitl we never stopped"
setup; APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=0 sh "$LAUNCHER"
nowant "systemctl stop xochitl"; nowant "systemctl start xochitl"

echo "case 4: lock contention refuses to start (no xochitl touch)"
setup; mkdir "$R/.lock"
APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER"; rc=$?
[ "$rc" = 1 ] || { echo "  FAIL: expected rc=1, got $rc"; fails=$((fails+1)); }
nowant "systemctl stop xochitl"; nowant "systemctl start xochitl"
[ -d "$R/.lock" ] || { echo "  FAIL: pre-existing lock was removed"; fails=$((fails+1)); }

echo "case 5: TERM mid-run restores xochitl"
setup; APP_MODE=hang PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER" &
LPID=$!; sleep 1; kill -TERM "$LPID" 2>/dev/null
for _ in 1 2 3 4 5 6; do grep -q "systemctl start xochitl" "$STUBLOG" && break; sleep 0.5; done
wait "$LPID" 2>/dev/null
want "systemctl start xochitl"

if [ "$fails" = 0 ]; then echo "launcher_test: OK"; else echo "launcher_test: $fails FAIL"; exit 1; fi
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `chmod +x tests/launcher_test.sh && bash tests/launcher_test.sh`
Expected: FAIL — `device/rmweb` and `device/rmweb-env.sh` don't exist yet (`sh: .../device/rmweb: No such file`, and `cp` of the env file errors).

- [ ] **Step 3: Write `device/rmweb-env.sh`** (env lifted verbatim from `run-wpeqt-on-device.sh:32-82`, minus dev-only diagnostics)

```sh
# rmweb production runtime env — the single source of truth, sourced by device/rmweb and the dev runner.
# Expects $R (bundle dir) set by the caller; falls back to RMWEB_ROOT / the default install path.
: "${R:=${RMWEB_ROOT:-/home/root/rmweb}}"

export LD_LIBRARY_PATH="$R/lib"
export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless   # llvmpipe = fast SW GL
export LIBGL_DRIVERS_PATH="$R/lib/dri" __EGL_VENDOR_LIBRARY_DIRS="$R/share/glvnd/egl_vendor.d"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_INJECTED_BUNDLE_PATH="$R/lib/wpe-webkit-2.0/injected-bundle"
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export WEBKIT_SKIA_CPU_PAINTING_THREADS=0
export WEBKIT_DISABLE_ASYNC_SCROLLING=1
export GIO_EXTRA_MODULES="$R/lib/gio/modules"          # glib-networking OpenSSL TLS backend -> https:// works
export FONTCONFIG_PATH=/etc/fonts HOME=/home/root
# JS: interpreter by default (JIT off). RMWEB_JIT=1 enables it with polling traps (see jit-works-polling-traps).
export JSC_useJIT="${RMWEB_JIT:-0}"
[ "${RMWEB_JIT:-0}" = 1 ] && export JSC_usePollingTraps=1
for opt in ${RMWEB_JSC_OPTS:-}; do export "$opt"; done  # extra JSC_* experiments (space-separated)
export RMWEB_BLOCK RMWEB_UA RMWEB_SITECSS               # content/UA/readability levers (pass-through to the app)
# Display path: epaper QPA, basic render loop (so afterRendering fires on the GUI thread), SW vblank timer.
export QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND="${RMWEB_QUICK_BACKEND:-epaper}" QSG_RENDER_LOOP=basic
export WEBKIT_FORCE_VBLANK_TIMER="${WEBKIT_FORCE_VBLANK_TIMER:-1}"
# On-screen URL keyboard (Qt Virtual Keyboard): IM module + bundle import/plugin paths EXTEND device defaults.
export QT_IM_MODULE=qtvirtualkeyboard
export QML_IMPORT_PATH="$R/qml" QML2_IMPORT_PATH="$R/qml"
export QT_PLUGIN_PATH="$R/plugins:${QT_PLUGIN_PATH:-/usr/lib/plugins}"
export RMWEB_PRESENT_DWELL RMWEB_DPR RMWEB_READER_FONT RMWEB_READER_DIR RMWEB_FULL_EVERY  # runtime levers
```

- [ ] **Step 4: Write `device/rmweb`** (the launcher)

```sh
#!/bin/sh
# rmweb — production launcher (runs ON the reMarkable Paper Pro). Stops xochitl, sets up the WPE runtime,
# runs the browser, and ALWAYS restores xochitl on exit (clean quit, crash, or kill) so the device is never
# left with a blank panel. See docs/superpowers/specs/2026-06-30-rmweb-phase5-packaging-design.md.
set -u
R="${RMWEB_ROOT:-/home/root/rmweb}"
LOCK="$R/.lock"
LOG="$R/rmweb.log"
log(){ echo "[rmweb] $*" >> "$LOG"; }

DONE=
cleanup(){
  [ -n "$DONE" ] && return; DONE=1
  [ -n "${APP:-}" ] && kill "$APP" 2>/dev/null                       # stop the app if still running
  for n in rmweb-wpeqt WPEWebProcess WPENetworkProc WPEGPUProcess; do # + any lingering WPE subprocesses
    for p in $(pgrep "$n" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
  done
  [ -n "${STOPPED:-}" ] && { log "restarting xochitl"; systemctl start xochitl; }  # home UI back FIRST
  [ -n "${MOUNTED:-}" ] && umount /usr/libexec 2>/dev/null
  rmdir "$LOCK" 2>/dev/null
}
trap 'cleanup' EXIT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 130' INT

# Single instance (mkdir is atomic on BusyBox). If the lock exists, another run owns xochitl — bail without
# touching it (DONE=1 so the EXIT trap's cleanup is a no-op and won't restart xochitl under the other run).
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "[rmweb] already running (or stale $LOCK; remove it if no rmweb is running)" >&2
  DONE=1; exit 1
fi

log "start"
if systemctl is-active --quiet xochitl; then log "stopping xochitl"; systemctl stop xochitl && STOPPED=1; fi

# WPE spawns helpers from the baked /usr/libexec/wpe-webkit-2.0 and / is read-only -> overlay it. Drop any
# stale overlay from a hard-killed prior run first, so we always mount our fresh helpers.
umount /usr/libexec 2>/dev/null || true
if [ ! -e /usr/libexec/wpe-webkit-2.0 ]; then
  rm -rf "$R/ovl"; mkdir -p "$R/ovl/upper/wpe-webkit-2.0" "$R/ovl/work"
  cp -a "$R/libexec/wpe-webkit-2.0/." "$R/ovl/upper/wpe-webkit-2.0/"
  mount -t overlay overlay -o lowerdir=/usr/libexec,upperdir="$R/ovl/upper",workdir="$R/ovl/work" /usr/libexec && MOUNTED=1
fi

. "$R/rmweb-env.sh"

log "launch rmweb-wpeqt $*"
"$R/bin/rmweb-wpeqt" "$@" >> "$LOG" 2>&1 &
APP=$!
wait "$APP"
log "rmweb-wpeqt exited rc=$?"
# EXIT trap -> cleanup -> restores xochitl.
```

- [ ] **Step 5: Make the launcher + env executable / runnable, then run the test to verify it passes**

Run: `chmod +x device/rmweb && bash tests/launcher_test.sh`
Expected: PASS — prints `case 1..5` then `launcher_test: OK`.

- [ ] **Step 6: Wire shell tests into `scripts/run-tests.sh`**

Replace the final summary block. Find:

```sh
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
```

with:

```sh
# Shell unit tests (launcher / installer no-brick logic) — pure bash + stubbed systemctl/mount.
for t in tests/*_test.sh; do
  [ -e "$t" ] || continue
  name="$(basename "$t" .sh)"
  if bash "$t"; then :; else echo "FAIL (shell): $name"; fail=1; fi
done
# Optional lint of the shipped shell (skip cleanly if shellcheck isn't installed).
if command -v shellcheck >/dev/null 2>&1; then
  shellcheck -s sh device/rmweb device/rmweb-env.sh device/install.sh 2>/dev/null || { echo "FAIL (shellcheck)"; fail=1; }
else
  echo "[tests] shellcheck not found — skipping shell lint"
fi
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
```

(Note: `device/install.sh` is created in Task 2; until then shellcheck on it is skipped by the `2>/dev/null || true` semantics — if shellcheck errors on a missing file the `||` branch sets fail. Run Task 1's shellcheck line only after Task 2, or temporarily drop `device/install.sh` from the shellcheck list. Simplest: implement Task 2 before relying on the shellcheck line; the `tests/*_test.sh` loop already passes in Task 1.)

- [ ] **Step 7: Run the full host test suite**

Run: `bash scripts/run-tests.sh`
Expected: existing C++ tests pass, then `launcher_test: OK`, then `ALL HOST TESTS OK`.

- [ ] **Step 8: Commit**

```bash
if git check-ignore -q .env; then
  git add device/rmweb device/rmweb-env.sh tests/launcher_test.sh scripts/run-tests.sh
  git commit -m "feat(pkg): on-device launcher + shared env, host no-brick tests

device/rmweb stops xochitl, overlay-mounts the WPE helpers, runs rmweb-wpeqt,
and a trap restores xochitl on every exit path (clean/crash/TERM). Env lifted
to device/rmweb-env.sh (shared, no drift). tests/launcher_test.sh stubs
systemctl/mount to prove the no-brick contract on the host.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 2: On-device installer + host test

**Files:**
- Create: `device/install.sh`, `tests/install_test.sh`

**Interfaces:**
- Produces: `device/install.sh` — run as `RMWEB_ROOT=<dir> sh device/install.sh`; exits non-zero if `bin/rmweb-wpeqt`, `rmweb`, or `rmweb-env.sh` are missing; writes `$RMWEB_ROOT/VERSION`; chmods the launcher; registers the rm-appload icon **only if** an apps dir + descriptor exist (layer B, added in Task 6).
- Consumes: the deployed bundle under `$RMWEB_ROOT`.

- [ ] **Step 1: Write the failing test** — `tests/install_test.sh`

```sh
#!/usr/bin/env bash
# Host tests for device/install.sh: integrity gate, chmod, version stamp. (Layer-B rm-appload registration
# is on-device-only and not exercised here.)
set -u
ROOT_REPO="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="$ROOT_REPO/device/install.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0

echo "case 1: missing app binary -> non-zero exit"
R="$TMP/a"; mkdir -p "$R"; cp "$ROOT_REPO/device/rmweb" "$ROOT_REPO/device/rmweb-env.sh" "$R/"
RMWEB_ROOT="$R" sh "$INSTALL" >/dev/null 2>&1; rc=$?
[ "$rc" != 0 ] || { echo "  FAIL: expected non-zero (no bin/rmweb-wpeqt)"; fails=$((fails+1)); }

echo "case 2: complete bundle -> success, version + executable"
R="$TMP/b"; mkdir -p "$R/bin"; : > "$R/bin/rmweb-wpeqt"
cp "$ROOT_REPO/device/rmweb" "$ROOT_REPO/device/rmweb-env.sh" "$ROOT_REPO/device/install.sh" "$R/"
RMWEB_ROOT="$R" sh "$INSTALL" >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] || { echo "  FAIL: expected success, got rc=$rc"; fails=$((fails+1)); }
[ -f "$R/VERSION" ] || { echo "  FAIL: VERSION not written"; fails=$((fails+1)); }
[ -x "$R/rmweb" ]   || { echo "  FAIL: launcher not executable"; fails=$((fails+1)); }

if [ "$fails" = 0 ]; then echo "install_test: OK"; else echo "install_test: $fails FAIL"; exit 1; fi
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `chmod +x tests/install_test.sh && bash tests/install_test.sh`
Expected: FAIL — `device/install.sh: No such file`.

- [ ] **Step 3: Write `device/install.sh`**

```sh
#!/bin/sh
# rmweb installer — run ON the device after deploying the bundle to /home/root/rmweb. Idempotent: safe to
# re-run, which is also the recovery step after a firmware/OTA update. See the Phase 5 design spec.
set -u
R="${RMWEB_ROOT:-/home/root/rmweb}"
VER="0.5.0"
fail(){ echo "[install] ERROR: $*" >&2; exit 1; }

# 1. Integrity: the launcher's hard dependencies must be present.
for f in bin/rmweb-wpeqt rmweb rmweb-env.sh; do
  [ -e "$R/$f" ] || fail "missing $R/$f — deploy the bundle first (scripts/bundle.sh)"
done

# 2. Make the launcher + installer executable.
chmod +x "$R/rmweb" "$R/install.sh" 2>/dev/null || true

# 3. Stamp the version.
echo "$VER" > "$R/VERSION"

# 4. Layer B: register with rm-appload if it's installed (its apps dir lives under /home, so it survives
#    reboot). Format/path verified on-device — degrade gracefully (Strategy A is unaffected) if absent.
APPLOAD_DIR=
for d in /home/root/.config/rm-appload/apps /opt/etc/draft /home/root/.entware/etc/draft; do
  [ -d "$d" ] && { APPLOAD_DIR="$d"; break; }
done
if [ -n "$APPLOAD_DIR" ] && [ -f "$R/appload/rmweb.draft" ]; then
  cp "$R/appload/rmweb.draft" "$APPLOAD_DIR/rmweb.draft"
  echo "[install] registered rmweb icon in $APPLOAD_DIR"
else
  echo "[install] rm-appload not found — skipping icon (launch with: $R/rmweb)"
fi

echo "[install] rmweb $VER installed under $R"
echo "[install] run:        $R/rmweb [URL]"
echo "[install] after OTA:   re-run  $R/install.sh"
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `chmod +x device/install.sh && bash tests/install_test.sh`
Expected: PASS — `install_test: OK`.

- [ ] **Step 5: Run the full host suite**

Run: `bash scripts/run-tests.sh`
Expected: `launcher_test: OK`, `install_test: OK`, `ALL HOST TESTS OK`.

- [ ] **Step 6: Commit**

```bash
if git check-ignore -q .env; then
  git add device/install.sh tests/install_test.sh
  git commit -m "feat(pkg): idempotent on-device installer + host test

Integrity-gates the bundle, chmods the launcher, stamps VERSION, and registers
the rm-appload icon when present. Re-runnable = the OTA-recovery procedure.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 3: In-app exit button (⏻) in the chrome bar

**Files:**
- Modify: `engine/wpeqt/main.cpp` (chrome `Hit` enum, layout constants, `hitChrome`, `drawChromeBar`, new `iconPower`, tap router)

**Interfaces:**
- Consumes: existing `WpeView::hitChrome(int,int)`, `drawChromeBar`, the `main()` tap router `switch (view->hitChrome(...))`.
- Produces: a new `WpeView::Power` hit; a far-right `⏻` button; tap → `std::_Exit(0)` (launcher then restores xochitl).

This change touches the hand-painted B2 chrome, which has no host unit tests (consistent with the rest of the chrome). Its test is a clean cross-compile now + on-device tap verification in Task 8.

- [ ] **Step 1: Ensure `<cstdlib>` is included** (for `std::_Exit`)

Check the includes at the top of `engine/wpeqt/main.cpp`. If `#include <cstdlib>` is absent, add it alongside the other C/C++ standard includes. (If present, skip.)

- [ ] **Step 2: Add `Power` to the `Hit` enum + `kPowerW` constant**

Find (line ~740):
```cpp
    enum Hit { None, Back, Fwd, Reload, Address, ZoomOut, ZoomIn, Reader };
```
Replace with:
```cpp
    enum Hit { None, Back, Fwd, Reload, Address, ZoomOut, ZoomIn, Reader, Power };
```

Find (line ~938):
```cpp
    static const int kBarH = 104, kBackX = 170, kFwdX = 340, kRelX = 560, kReaderW = 190, kZoomW = 120;
```
Replace with:
```cpp
    static const int kBarH = 104, kBackX = 170, kFwdX = 340, kRelX = 560, kReaderW = 190, kZoomW = 120, kPowerW = 130;
```

- [ ] **Step 3: Hit-test the `⏻` zone** — update `hitChrome`

Find:
```cpp
    Hit hitChrome(int x, int y) const {
        if (!m_chromeOn || y >= kBarH) return None;
        const int readerX = int(width()) - kReaderW;       // right cluster:  A- | A+ | Reader
        const int zInX = readerX - kZoomW, zOutX = zInX - kZoomW;
        if (x < kBackX)   return Back;
        if (x < kFwdX)    return Fwd;
        if (x < kRelX)    return Reload;
        if (x >= readerX) return Reader;
        if (x >= zInX)    return ZoomIn;
        if (x >= zOutX)   return ZoomOut;
        return Address;
    }
```
Replace with:
```cpp
    Hit hitChrome(int x, int y) const {
        if (!m_chromeOn || y >= kBarH) return None;
        const int powerX  = int(width()) - kPowerW;         // right cluster:  A- | A+ | Reader | Power
        const int readerX = powerX - kReaderW;
        const int zInX = readerX - kZoomW, zOutX = zInX - kZoomW;
        if (x < kBackX)   return Back;
        if (x < kFwdX)    return Fwd;
        if (x < kRelX)    return Reload;
        if (x >= powerX)  return Power;
        if (x >= readerX) return Reader;
        if (x >= zInX)    return ZoomIn;
        if (x >= zOutX)   return ZoomOut;
        return Address;
    }
```

- [ ] **Step 4: Draw the `⏻` button** — update `drawChromeBar` and add `iconPower`

Find:
```cpp
        const qreal readerX = w - kReaderW, zInX = readerX - kZoomW, zOutX = zInX - kZoomW;
```
Replace with:
```cpp
        const qreal powerX = w - kPowerW, readerX = powerX - kReaderW, zInX = readerX - kZoomW, zOutX = zInX - kZoomW;
```

Find (the end of `drawChromeBar`, the reader branch):
```cpp
        } else { pen(m_readerable); iconReader(p, rcx, cy); }
    }
```
Replace with:
```cpp
        } else { pen(m_readerable); iconReader(p, rcx, cy); }
        pen(true); iconPower(p, powerX + kPowerW / 2.0, cy);   // exit to the reMarkable menu
    }
    void iconPower(QPainter *p, qreal cx, qreal cy) const {                // power symbol: ring (gap at top) + bar
        QPen pn = p->pen(); pn.setWidthF(5); pn.setCapStyle(Qt::RoundCap); p->setPen(pn); p->setBrush(Qt::NoBrush);
        const qreal r = 17;
        p->drawArc(QRectF(cx - r, cy - r, 2 * r, 2 * r), 120 * 16, 300 * 16);   // open at the top
        p->drawLine(QPointF(cx, cy - r - 5), QPointF(cx, cy - 1));              // the "I" through the gap
    }
```

- [ ] **Step 5: Route the `Power` tap to a clean exit** — update the `main()` tap router

Find:
```cpp
                    case WpeView::Address: view->beginEdit();  return;   // open the on-screen URL keyboard
                    case WpeView::None:    break;             // tap not on the bar
```
Replace with:
```cpp
                    case WpeView::Address: view->beginEdit();  return;   // open the on-screen URL keyboard
                    case WpeView::Power:   std::_Exit(0);      return;   // quit to menu; launcher restores xochitl
                    case WpeView::None:    break;             // tap not on the bar
```

- [ ] **Step 6: Cross-compile to verify it builds**

Run: `./scripts/build-wpeqt.sh`
Expected: builds clean → `build/rmweb-wpeqt` produced, no warnings about an unhandled `Power` enum case.

- [ ] **Step 7: Commit**

```bash
if git check-ignore -q .env; then
  git add engine/wpeqt/main.cpp
  git commit -m "feat(shell): exit button (power glyph) -> clean quit to the reMarkable menu

A far-right power button in the B2 chrome; tap calls std::_Exit(0) (skips the
WebKit teardown SIGABRT) so the launcher's trap restores xochitl.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 4: Ship the new files in the bundle

**Files:**
- Modify: `scripts/bundle.sh`

**Interfaces:**
- Consumes: `device/rmweb`, `device/rmweb-env.sh`, `device/install.sh` (and, when Task 6 lands, `device/appload/`, `device/icon.svg`).
- Produces: those files at the bundle root on the device, launcher + installer executable, `VERSION` stamped.

- [ ] **Step 1: Add the copy step to `scripts/bundle.sh`**

Find:
```sh
cp -a build/wpe_render              "$B/bin/"

echo "[bundle] local size:"; du -sh "$B"
```
Replace with:
```sh
cp -a build/wpe_render              "$B/bin/"
# Phase 5 — installable app: on-device launcher, shared env, installer, version stamp, and (layer B) the
# rm-appload descriptor + icon. These live under device/ in the repo and ship at the bundle root.
cp -a device/rmweb device/rmweb-env.sh device/install.sh "$B/"
chmod +x "$B/rmweb" "$B/install.sh"
echo "0.5.0" > "$B/VERSION"
[ -d device/appload ] && cp -a device/appload "$B/"
[ -f device/icon.svg ] && cp -a device/icon.svg "$B/"

echo "[bundle] local size:"; du -sh "$B"
```

- [ ] **Step 2: Verify the staging copy works (no device needed)**

Run:
```bash
mkdir -p /tmp/rmweb-b/bin && B=/tmp/rmweb-b bash -c '
  cp -a device/rmweb device/rmweb-env.sh device/install.sh "$B/" &&
  chmod +x "$B/rmweb" "$B/install.sh" && echo "0.5.0" > "$B/VERSION" &&
  ls -l "$B"/rmweb "$B"/rmweb-env.sh "$B"/install.sh "$B"/VERSION'
```
Expected: all four listed, `rmweb` + `install.sh` executable (`-rwx`).

- [ ] **Step 3: Lint the script**

Run: `command -v shellcheck >/dev/null && shellcheck scripts/bundle.sh || echo "shellcheck absent — skip"`
Expected: no new errors (pre-existing style warnings tolerated).

- [ ] **Step 4: Commit**

```bash
if git check-ignore -q .env; then
  git add scripts/bundle.sh
  git commit -m "build(pkg): ship launcher, env, installer, VERSION in the bundle

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 5: Refactor the dev runner to reuse the shared env (DRY)

**Files:**
- Modify: `scripts/run-wpeqt-on-device.sh`

**Interfaces:**
- Consumes: `device/rmweb-env.sh` (now deployed in the bundle at `$R/rmweb-env.sh`).
- Produces: the dev `show`/`save`/`bench` flow unchanged in behaviour, but the production env comes from the shared file (no second copy to drift).

The dev runner keeps its `show` orchestration (it adds `SHOW_SECS` timed-kill + diagnostic env that the production launcher intentionally lacks). Only the inlined env exports are replaced by sourcing the shared file. The overlay setup and xochitl stop/kill stay (they serve `save`/`bench` too).

- [ ] **Step 1: Source the shared env in place of the inline block**

Find (`scripts/run-wpeqt-on-device.sh:32-57`, the common export block from `export LD_LIBRARY_PATH=...` through `export RMWEB_SITECSS ...`) and replace the whole block with:

```sh
# Production runtime env — single source of truth (shared with the on-device launcher device/rmweb).
. "$R/rmweb-env.sh"
```

- [ ] **Step 2: Drop the now-duplicated show-mode display/keyboard exports**

In the `if [ "$MODE" = show ]; then` branch, the env that `rmweb-env.sh` now sets is duplicated. Remove these lines from that branch (they are already exported by the sourced file): the `QT_QPA_PLATFORM=epaper ... QSG_RENDER_LOOP=basic` line, the `QT_IM_MODULE=qtvirtualkeyboard` line, the `QML_IMPORT_PATH ... QML2_IMPORT_PATH` line, the `QT_PLUGIN_PATH=...` line, and the `WEBKIT_FORCE_VBLANK_TIMER=...` line. **Keep** the diagnostic passthroughs (`RMWEB_AUTOPAGE_MS`, `RMWEB_DEBUG_*`, `RMWEB_GRAB_MS`, `RMWEB_PRESENT_DWELL`, `RMWEB_DPR`, `RMWEB_READER_FONT`, `RMWEB_READER_DIR`, `RMWEB_FULL_EVERY`) and the `systemctl stop xochitl && STOPPED=1` line and the run/kill block.

(For `bench`/`save` the sourced env sets `QT_QPA_PLATFORM=epaper`; those branches already override it with `export QT_QPA_PLATFORM=offscreen ...` AFTER this point in the script — verify the override still follows the source. It does: the source is near the top, the mode branches come after.)

- [ ] **Step 3: Verify the script still parses and lints**

Run: `bash -n scripts/run-wpeqt-on-device.sh && (command -v shellcheck >/dev/null && shellcheck scripts/run-wpeqt-on-device.sh || echo "shellcheck absent — skip")`
Expected: no syntax errors; no new shellcheck errors.

- [ ] **Step 4: Commit**

```bash
if git check-ignore -q .env; then
  git add scripts/run-wpeqt-on-device.sh
  git commit -m "refactor(dev): source the shared rmweb-env.sh (one env, no drift)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 6: Layer B — rm-appload icon (scaffold + on-device verify)

**Files:**
- Create: `device/appload/rmweb.draft`, `device/icon.svg`

**Interfaces:**
- Consumes: `device/install.sh` step 4 (already written in Task 2), `scripts/bundle.sh` layer-B copy (already written in Task 4).
- Produces: a tappable rmweb entry in rm-appload whose `call` runs `/home/root/rmweb/rmweb`.

The descriptor format and the apps-directory path are **verified on the device** (rm-appload version-specific). The content below is a draft-format starting point; adjust on-device in Task 8 if the installed rm-appload expects a different schema. Strategy A is unaffected if this is wrong/absent.

- [ ] **Step 1: Write `device/appload/rmweb.draft`**

```ini
name=rmweb
desc=Web browser (WPE WebKit)
call=/home/root/rmweb/rmweb
term=:
imgFile=icon
```

- [ ] **Step 2: Write `device/icon.svg`** (a simple book-with-globe glyph; converted to the raster rm-appload wants on-device)

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <rect width="128" height="128" rx="20" fill="#ffffff" stroke="#000000" stroke-width="4"/>
  <circle cx="64" cy="58" r="34" fill="none" stroke="#000000" stroke-width="5"/>
  <path d="M30 58 H98 M64 24 V92 M40 38 Q64 52 88 38 M40 78 Q64 64 88 78" fill="none" stroke="#000000" stroke-width="4"/>
  <rect x="34" y="98" width="60" height="10" rx="3" fill="#000000"/>
</svg>
```

- [ ] **Step 3: Verify the descriptor + icon are picked up by the bundle staging**

Run:
```bash
B=/tmp/rmweb-b bash -c '[ -d device/appload ] && cp -a device/appload "$B/"; [ -f device/icon.svg ] && cp -a device/icon.svg "$B/"; ls -R "$B/appload" "$B/icon.svg"'
```
Expected: `appload/rmweb.draft` and `icon.svg` listed.

- [ ] **Step 4: Commit**

```bash
if git check-ignore -q .env; then
  git add device/appload/rmweb.draft device/icon.svg
  git commit -m "feat(pkg): rm-appload descriptor + icon (layer B; format verified on-device)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 7: Docs — install / run / OTA recovery

**Files:**
- Create: `docs/install.md`

- [ ] **Step 1: Write `docs/install.md`**

````markdown
# Installing rmweb on the reMarkable Paper Pro

rmweb installs entirely under `/home/root/rmweb` (the only writable, OTA-surviving location). It never
modifies `/etc` or the rootfs, and it never disables xochitl — it only stops it while the browser is on
screen and restarts it on exit.

## First install (from the dev host)

```sh
# 1. Build the app and assemble + deploy the bundle (stops nothing; just copies files):
./scripts/build-wpeqt.sh
./scripts/bundle.sh
# 2. Wire it up on the device (idempotent):
ssh root@10.11.99.1 '/home/root/rmweb/install.sh'
```

## Running

- From the device shell:  `/home/root/rmweb/rmweb [URL]`
- From the home screen:    tap the **rmweb** icon (requires XOVI + rm-appload; see below).

The browser takes over the screen (xochitl is stopped). Tap the **⏻** button at the right of the toolbar
to quit — xochitl (your normal reMarkable UI) comes back automatically. xochitl is always restored on
exit, crash, or kill; a reboot always restores it too.

## Home-screen icon (optional, layer B)

The icon needs the community launcher **XOVI + rm-appload** installed on the device. With it present,
`install.sh` registers rmweb automatically. Without it, rmweb still runs from the shell (above).

## After a firmware update (OTA)

The bundle under `/home/root/rmweb` survives OTA, but re-run the installer to re-assert the icon hook:

```sh
ssh root@10.11.99.1 '/home/root/rmweb/install.sh'
```

If the home-screen icon is gone after an OTA, reinstall XOVI/rm-appload, then re-run `install.sh`.

## Logs

Runtime output goes to `/home/root/rmweb/rmweb.log` (kept under `/home` so it survives a watchdog reboot).
````

- [ ] **Step 2: Commit**

```bash
if git check-ignore -q .env; then
  git add docs/install.md
  git commit -m "docs: install / run / OTA-recovery guide

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 8: On-device integration verification + review checkpoint

**Files:** none (verification + review). Requires the device online (`ssh root@10.11.99.1`).

- [ ] **Step 1: Deploy + install**

```bash
./scripts/build-wpeqt.sh && ./scripts/bundle.sh
ssh root@10.11.99.1 '/home/root/rmweb/install.sh'
```
Expected: installer prints `rmweb 0.5.0 installed under /home/root/rmweb`.

- [ ] **Step 2: Launch from the device shell + exit via the button**

```bash
ssh root@10.11.99.1 '/home/root/rmweb/rmweb https://en.wikipedia.org/wiki/E_Ink'
```
Tap the **⏻** button. Expected: the page renders on e-ink; tapping ⏻ quits and the reMarkable home UI
(xochitl) returns within a couple of seconds. Verify: `ssh root@10.11.99.1 'systemctl is-active xochitl'` → `active`.

- [ ] **Step 3: No-brick checks**

- Kill mid-run: launch, then `ssh root@10.11.99.1 'kill $(pgrep -f /home/root/rmweb/rmweb | head -n1)'`; confirm xochitl returns (`systemctl is-active xochitl` → `active`).
- Lock: while running, a second `/home/root/rmweb/rmweb` prints "already running" and exits without disturbing the first.
- Reboot: `ssh root@10.11.99.1 reboot`; after it comes up, xochitl is normal, `/home/root/rmweb` is intact, and `rmweb` relaunches.

- [ ] **Step 4: Layer B (if XOVI/rm-appload is installed)**

Confirm the **rmweb** icon appears in the rm-appload launcher and tapping it starts the app. If the
descriptor format/path differs from `device/appload/rmweb.draft`, fix it on-device, mirror the fix back
into the repo file, rebundle, and re-commit (guarded). If rm-appload isn't installed, note it and move on
(Strategy A is complete).

- [ ] **Step 5: Code-review subagent**

Dispatch a `feature-dev:code-reviewer` subagent over the Phase 5 diff (the launcher, installer, env, the
`main.cpp` exit-button change, bundle/runner edits). Apply any high-confidence fixes; re-run
`bash scripts/run-tests.sh`.

- [ ] **Step 6: Simplify subagent**

Dispatch a `code-simplifier:code-simplifier` subagent over the same diff. Apply safe simplifications;
re-run `bash scripts/run-tests.sh`.

- [ ] **Step 7: Mark Phase 5 done**

Update `CLAUDE.md` status (Phase 5 ✅: installable app — on-device launcher with guaranteed xochitl
restore, idempotent installer, OTA survival, ⏻ exit, rm-appload icon) and the task tracker.

```bash
if git check-ignore -q .env; then
  git add CLAUDE.md docs/install.md
  git commit -m "docs: mark Phase 5 (packaging) done + on-device notes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

## Self-Review

**Spec coverage:**
- Launcher (Strategy A) → Task 1. ✓
- `/home` install layout + installer → Tasks 2, 4. ✓
- Reboot/OTA survival → install.sh idempotent (Task 2) + on-device reboot check (Task 8). ✓
- rm-appload icon (Strategy B) → Tasks 2 (registration), 6 (descriptor/icon), 8 (verify). ✓
- In-app exit / no-brick → Task 1 (trap) + Task 3 (⏻ → `_Exit`). ✓
- Shared env / dev-runner DRY → Tasks 1, 5. ✓
- Host tests for the no-brick contract → Tasks 1, 2. ✓
- Docs → Task 7. ✓
- code-review + simplify checkpoint → Task 8. ✓
- Out of scope (suspend/resume, GitHub publish) → not planned, matching the spec. ✓

**Placeholder scan:** rm-appload descriptor format + apps-dir path are explicitly on-device-verified (Tasks 6/8), not silent TBDs; concrete starting content is provided. No other placeholders.

**Type/name consistency:** `Hit::Power`, `kPowerW`, `iconPower`, `powerX` are introduced together in Task 3 and used consistently. `device/rmweb-env.sh` is created in Task 1 and referenced by Tasks 5 (`. "$R/rmweb-env.sh"`) and the launcher. `RMWEB_ROOT` is the single root override across launcher, installer, and both tests. `VERSION` value `0.5.0` matches between `install.sh` and `bundle.sh`.
