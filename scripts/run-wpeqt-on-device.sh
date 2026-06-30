#!/usr/bin/env bash
set -euo pipefail
# Run the Qt6+WPE app on the device.
#   MODE=save (default): QT offscreen -> /home/root/rmweb/qt-out.png, copied back (3b.1/3b.2 proof).
#   MODE=show          : QT epaper -> e-ink, xochitl stopped/restored (3b.3, needs the display path).
# Usage: scripts/run-wpeqt-on-device.sh [save|show] [URL]
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"; DUSER="${DEVICE_USER:-root}"
MODE="${1:-save}"; URL="${2:-}"

scp -q build/rmweb-wpeqt "$DUSER@$HOST:/home/root/rmweb/bin/rmweb-wpeqt"

ssh "$DUSER@$HOST" "MODE='$MODE' URL='$URL' SHOW_SECS='${SHOW_SECS:-40}' RMWEB_AUTOPAGE_MS='${RMWEB_AUTOPAGE_MS:-}' WEBKIT_DEBUG='${WEBKIT_DEBUG:-}' RMWEB_FULL_EVERY='${RMWEB_FULL_EVERY:-}' RMWEB_DEBUG_TAP='${RMWEB_DEBUG_TAP:-}' RMWEB_DEBUG_READER='${RMWEB_DEBUG_READER:-}' RMWEB_DEBUG_KB='${RMWEB_DEBUG_KB:-}' RMWEB_JIT='${RMWEB_JIT:-}' RMWEB_JSC_OPTS='${RMWEB_JSC_OPTS:-}' RMWEB_BLOCK='${RMWEB_BLOCK:-}' RMWEB_QUICK_BACKEND='${RMWEB_QUICK_BACKEND:-}' RMWEB_GRAB_MS='${RMWEB_GRAB_MS:-}' RMWEB_MANUAL_PRESENT='${RMWEB_MANUAL_PRESENT:-}' RMWEB_PRESENT_DWELL='${RMWEB_PRESENT_DWELL:-}' RMWEB_DPR='${RMWEB_DPR:-}' RMWEB_READER_FONT='${RMWEB_READER_FONT:-}' RMWEB_READER_DIR='${RMWEB_READER_DIR:-}' bash -s" <<'EOS'
set -e
R=/home/root/rmweb
# WPE spawns helpers from the baked /usr/libexec/wpe-webkit-2.0 and / is read-only -> overlay it.
# Install the cleanup trap BEFORE mounting/stopping anything, so a failure in the setup block
# still restores xochitl + unmounts the overlay (the guard vars default to unset = no-op).
cleanup(){ [ -n "${STOPPED:-}" ] && systemctl start xochitl; [ -n "${MOUNTED:-}" ] && umount /usr/libexec 2>/dev/null; }
trap cleanup EXIT
# A hard-killed prior run leaves a stale overlay (its EXIT trap never ran) whose upper holds the OLD
# helpers — and the [ ! -e ] check below would then skip re-mounting and silently run stale binaries.
# Drop any stale overlay first so we always mount fresh (harmless/ignored if nothing is mounted there).
umount /usr/libexec 2>/dev/null || true
if [ ! -e /usr/libexec/wpe-webkit-2.0 ]; then
  rm -rf "$R/ovl"; mkdir -p "$R/ovl/upper/wpe-webkit-2.0" "$R/ovl/work"
  cp -a "$R/libexec/wpe-webkit-2.0/." "$R/ovl/upper/wpe-webkit-2.0/"
  mount -t overlay overlay -o lowerdir=/usr/libexec,upperdir="$R/ovl/upper",workdir="$R/ovl/work" /usr/libexec && MOUNTED=1
fi

export LD_LIBRARY_PATH="$R/lib"
export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless   # llvmpipe = multi-core+SIMD SW GL (~64x faster than softpipe; see six-second-render memory)
export LIBGL_DRIVERS_PATH="$R/lib/dri" __EGL_VENDOR_LIBRARY_DIRS="$R/share/glvnd/egl_vendor.d"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_INJECTED_BUNDLE_PATH="$R/lib/wpe-webkit-2.0/injected-bundle"
# CPU-only rendering knobs (verified against WPE 2.48.5 source — see docs/research/wpe-rendering-protocol.md):
#   ENABLE_CPU_RENDERING=1  -> explicit software Skia (no Skia-GL context).
#   CPU_PAINTING_THREADS=0  -> paint synchronously on the main thread; bypasses the threaded-Skia WorkerPool
#                              whose multi-region replay segfaulted when rendering scrolled (scrollY>0) frames.
#   DISABLE_ASYNC_SCROLLING -> keep scrolling on the main thread (calmer repaints, fine for paged e-ink).
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export WEBKIT_SKIA_CPU_PAINTING_THREADS=0
export WEBKIT_DISABLE_ASYNC_SCROLLING=1
export GIO_EXTRA_MODULES="$R/lib/gio/modules"   # glib-networking OpenSSL TLS backend -> https:// works
export FONTCONFIG_PATH=/etc/fonts HOME=/home/root
# The device forbids writable+executable (W^X) / MAP_JIT memory, so JavaScriptCore's JIT segfaults
# the WebProcess the moment a page runs JS. Run JSC in its interpreter (LLInt) — stable, fine for e-ink.
export JSC_useJIT="${RMWEB_JIT:-0}"   # RMWEB_JIT=1 enables the JIT (works — see usePollingTraps below)
# THE JIT FIX (verified 2026-06-27): the JIT abort was a SIGNAL conflict — JSC's signal-based VMTraps vs our
# crash handler / the environment. JSC_usePollingTraps=1 makes JSC poll instead of using signals -> no abort,
# and the JIT renders correctly + fast (Wikipedia content rendered, was blank/aborting before). Bake it in.
[ "${RMWEB_JIT:-0}" = 1 ] && export JSC_usePollingTraps=1
[ -n "${RMWEB_JSC_OPTS:-}" ] && export $RMWEB_JSC_OPTS   # extra JSC_* options for experiments (space-separated)
export RMWEB_BLOCK   # content-blocking: unset/!=0 => on (drop third-party scripts/ads); RMWEB_BLOCK=0 => off

if [ "$MODE" = show ]; then
  echo "[device] stopping xochitl"; systemctl stop xochitl && STOPPED=1
  # epaper scenegraph, but the BASIC render loop so QQuickWindow::afterRendering fires on the GUI thread
  # and the epaper EPRenderLoop's slow auto-present is bypassed — we present each frame via EpaperRefresh.
  export QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND="${RMWEB_QUICK_BACKEND:-epaper}" QSG_RENDER_LOOP=basic
  # Qt Virtual Keyboard for on-screen URL entry: IM module + bundle import/plugin paths. These EXTEND the
  # device defaults, so QtQuick.Controls and the epaper platform plugin still resolve from /usr/lib.
  export QT_IM_MODULE=qtvirtualkeyboard
  export QML_IMPORT_PATH="$R/qml" QML2_IMPORT_PATH="$R/qml"
  export QT_PLUGIN_PATH="$R/plugins:${QT_PLUGIN_PATH:-/usr/lib/plugins}"
  export RMWEB_AUTOPAGE_MS   # diagnostic: if set, the app auto-turns pages at this interval (ms)
  export RMWEB_PRESENT_DWELL # A6: min present spacing (ms) for the frameSwapped-gated serializer
  export RMWEB_DPR           # readability: device-pixel-ratio (CSS viewport = panel/dpr); ~2.0 = readable
  export RMWEB_READER_FONT   # reader mode: base font size px in the reflowed column (default 38; live-tunable)
  export RMWEB_READER_DIR    # reader mode: dir with vendored Readability.js (default /home/root/rmweb/share/reader)
  export RMWEB_FULL_EVERY    # e-ink: full colour anti-ghost flash every N page-turns (<=0 = grayscale only, least flicker)
  export RMWEB_DEBUG_TAP     # diagnostic: fire one synthetic click into the debug box (touch->mouse bridge proof)
  export RMWEB_DEBUG_READER  # diagnostic: auto-toggle reader mode once after N ms (verify reflow w/ RMWEB_GRAB_MS)
  export RMWEB_DEBUG_KB      # diagnostic: open the URL keyboard after N ms (grab its rendering w/ RMWEB_GRAB_MS)
  # Force WebKit's 60 fps software vblank timer instead of the DRM hardware vblank: the headless view may
  # still bind the e-ink panel's DRM CRTC, whose vblank ticks at the panel's slow rate (~0.16 Hz => the
  # observed ~6 s render cadence). The timer monitor decouples WebKit's frame clock from the panel.
  export WEBKIT_FORCE_VBLANK_TIMER="${WEBKIT_FORCE_VBLANK_TIMER:-1}"
  echo "[device] showing on e-ink for ${SHOW_SECS}s ..."
  # BusyBox here has no `timeout`; run in the background and kill after SHOW_SECS.
  "$R/bin/rmweb-wpeqt" "$URL" >"$R/wpeqt.log" 2>&1 &
  APP=$!
  sleep "$SHOW_SECS"
  # Robust teardown so a HUNG app can't leave the panel frozen (xochitl stopped): SIGTERM, then SIGKILL the
  # app AND its WPE subprocess children (a stuck WebProcess holds the binary + DRM). The EXIT trap restores
  # xochitl. BusyBox has no pkill/timeout, so loop pgrep+kill.
  kill "$APP" 2>/dev/null || true
  for i in 1 2 3; do kill -0 "$APP" 2>/dev/null || break; sleep 1; done
  for n in rmweb-wpeqt WPEWebProcess WPENetworkProc WPEGPUProcess; do
    for p in $(pgrep "$n" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
  done
  tail -n 30 "$R/wpeqt.log"
elif [ "$MODE" = bench ]; then
  # Isolation probe: run the engine display path OFFSCREEN (no epaper, no panel, xochitl untouched) so the
  # buffer-rendered cadence can be measured with the e-ink present path entirely out of the picture.
  export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
  export RMWEB_AUTOPAGE_MS WEBKIT_DEBUG WEBKIT_FORCE_VBLANK_TIMER="${WEBKIT_FORCE_VBLANK_TIMER:-1}"
  echo "[device] bench (offscreen, no epaper) for ${SHOW_SECS}s ..."
  "$R/bin/rmweb-wpeqt" "$URL" >"$R/wpeqt.log" 2>&1 &
  APP=$!
  sleep "$SHOW_SECS"
  kill "$APP" 2>/dev/null || true
  wait "$APP" 2>/dev/null || true
  tail -n 40 "$R/wpeqt.log"
else
  export QT_QPA_PLATFORM=offscreen
  rm -f "$R/qt-out.png"
  "$R/bin/rmweb-wpeqt" "$URL" "$R/qt-out.png" 2>&1 | tail -30 || echo "[device] rc=$?"
  echo "[device] result: $(ls -l "$R/qt-out.png" 2>&1)"
fi
EOS

if [ "$MODE" != show ]; then
  echo "[host] copying device PNG -> build/qt-out-device.png"
  scp -q "$DUSER@$HOST:/home/root/rmweb/qt-out.png" build/qt-out-device.png 2>/dev/null \
    && file build/qt-out-device.png || { echo "no PNG produced"; exit 1; }
fi
