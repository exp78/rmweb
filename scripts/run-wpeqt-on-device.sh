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

ssh "$DUSER@$HOST" "MODE='$MODE' URL='$URL' SHOW_SECS='${SHOW_SECS:-40}' RMWEB_AUTOPAGE_MS='${RMWEB_AUTOPAGE_MS:-}' WEBKIT_DEBUG='${WEBKIT_DEBUG:-}' RMWEB_FULL_EVERY='${RMWEB_FULL_EVERY:-}' RMWEB_DEBUG_TAP='${RMWEB_DEBUG_TAP:-}' RMWEB_DEBUG_READER='${RMWEB_DEBUG_READER:-}' RMWEB_DEBUG_KB='${RMWEB_DEBUG_KB:-}' RMWEB_DEBUG_ZOOM='${RMWEB_DEBUG_ZOOM:-}' RMWEB_JIT='${RMWEB_JIT:-}' RMWEB_JSC_OPTS='${RMWEB_JSC_OPTS:-}' RMWEB_BLOCK='${RMWEB_BLOCK:-}' RMWEB_SITECSS='${RMWEB_SITECSS:-}' RMWEB_QUICK_BACKEND='${RMWEB_QUICK_BACKEND:-}' RMWEB_GRAB_MS='${RMWEB_GRAB_MS:-}' RMWEB_MANUAL_PRESENT='${RMWEB_MANUAL_PRESENT:-}' RMWEB_PRESENT_DWELL='${RMWEB_PRESENT_DWELL:-}' RMWEB_DPR='${RMWEB_DPR:-}' RMWEB_READER_FONT='${RMWEB_READER_FONT:-}' RMWEB_READER_DIR='${RMWEB_READER_DIR:-}' RMWEB_UA='${RMWEB_UA:-}' bash -s" <<'EOS'
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

# Production runtime env — single source of truth (shared with the on-device launcher device/rmweb).
. "$R/rmweb-env.sh"

if [ "$MODE" = show ]; then
  echo "[device] stopping xochitl"; systemctl stop xochitl && STOPPED=1
  export RMWEB_AUTOPAGE_MS   # diagnostic: if set, the app auto-turns pages at this interval (ms)
  export RMWEB_PRESENT_DWELL # A6: min present spacing (ms) for the frameSwapped-gated serializer
  export RMWEB_DPR           # readability: device-pixel-ratio (CSS viewport = panel/dpr); ~2.0 = readable
  export RMWEB_READER_FONT   # reader mode: base font size px in the reflowed column (default 38; live-tunable)
  export RMWEB_READER_DIR    # reader mode: dir with vendored Readability.js (default /home/root/rmweb/share/reader)
  export RMWEB_FULL_EVERY    # e-ink: full colour anti-ghost flash every N page-turns (<=0 = grayscale only, least flicker)
  export RMWEB_DEBUG_TAP     # diagnostic: fire one synthetic click into the debug box (touch->mouse bridge proof)
  export RMWEB_DEBUG_READER  # diagnostic: auto-toggle reader mode once after N ms (verify reflow w/ RMWEB_GRAB_MS)
  export RMWEB_DEBUG_KB      # diagnostic: open the URL keyboard after N ms (grab its rendering w/ RMWEB_GRAB_MS)
  export RMWEB_DEBUG_ZOOM    # diagnostic: bump page zoom +2 steps after N ms (verify scaling w/ RMWEB_GRAB_MS)
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
