#!/usr/bin/env bash
set -euo pipefail
# Run the Qt6+WPE app on the device.
#   MODE=save (default): QT offscreen -> /home/root/rmweb/qt-out.png, copied back (3b.1/3b.2 proof).
#   MODE=show          : QT epaper -> e-ink, xochitl stopped/restored (3b.3, needs the display path).
# Usage: scripts/run-wpeqt-on-device.sh [save|show] [URL]
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"; DUSER="${REMARKABLE_USER:-${DEVICE_USER:-root}}"   # REMARKABLE_USER (.env) is canonical; DEVICE_USER = legacy fallback
MODE="${1:-save}"; URL="${2:-}"

# Upload to a temp name, then atomic mv into place: overwriting a RUNNING binary in place fails with
# ETXTBSY, and a mid-scp disconnect must not leave a half-written binary behind.
scp -q build/rmweb-wpeqt "$DUSER@$HOST:/home/root/rmweb/bin/.rmweb-wpeqt.new"
ssh "$DUSER@$HOST" 'mv -f /home/root/rmweb/bin/.rmweb-wpeqt.new /home/root/rmweb/bin/rmweb-wpeqt'

# SHOW_SECS: seconds to leave the app on e-ink (default 180). Use 0 to run until Ctrl-C / kill.
# Shell-escape every value interpolated into the remote command (run-on-device.sh:13 pattern) —
# an apostrophe in a URL/var must not break out of quoting: this ssh session runs as root.
REMOTE_ENV="$(printf '%q ' \
  "MODE=$MODE" "URL=$URL" "SHOW_SECS=${SHOW_SECS:-180}" \
  "RMWEB_AUTOPAGE_MS=${RMWEB_AUTOPAGE_MS:-}" "WEBKIT_DEBUG=${WEBKIT_DEBUG:-}" \
  "RMWEB_FULL_EVERY=${RMWEB_FULL_EVERY:-}" "RMWEB_DEBUG_TAP=${RMWEB_DEBUG_TAP:-}" \
  "RMWEB_DEBUG_READER=${RMWEB_DEBUG_READER:-}" "RMWEB_DEBUG_KB=${RMWEB_DEBUG_KB:-}" \
  "RMWEB_DEBUG_ZOOM=${RMWEB_DEBUG_ZOOM:-}" "RMWEB_JIT=${RMWEB_JIT:-}" "RMWEB_JSC_OPTS=${RMWEB_JSC_OPTS:-}" \
  "RMWEB_BLOCK=${RMWEB_BLOCK:-}" "RMWEB_SITECSS=${RMWEB_SITECSS:-}" "RMWEB_QUICK_BACKEND=${RMWEB_QUICK_BACKEND:-}" \
  "RMWEB_GRAB_MS=${RMWEB_GRAB_MS:-}" "RMWEB_MANUAL_PRESENT=${RMWEB_MANUAL_PRESENT:-}" \
  "RMWEB_PRESENT_DWELL=${RMWEB_PRESENT_DWELL:-}" "RMWEB_DPR=${RMWEB_DPR:-}" \
  "RMWEB_READER_FONT=${RMWEB_READER_FONT:-}" "RMWEB_READER_DIR=${RMWEB_READER_DIR:-}" "RMWEB_UA=${RMWEB_UA:-}" \
  "RMWEB_DEBUG_FIND=${RMWEB_DEBUG_FIND:-}" "RMWEB_DEBUG_PROBE=${RMWEB_DEBUG_PROBE:-}" \
  "RMWEB_DEBUG_FORM=${RMWEB_DEBUG_FORM:-}" "RMWEB_DEBUG_SEARCH=${RMWEB_DEBUG_SEARCH:-}" \
  "RMWEB_NOJS=${RMWEB_NOJS:-}" "RMWEB_DEBUG_UITAP=${RMWEB_DEBUG_UITAP:-}" \
  "RMWEB_AUTOREFRESH_MS=${RMWEB_AUTOREFRESH_MS:-}" \
  "QT_LOGGING_RULES=${QT_LOGGING_RULES:-}")"
ssh "$DUSER@$HOST" "$REMOTE_ENV bash -s" <<'EOS'
set -e
R=/home/root/rmweb
# Same single-instance discipline as the production launcher (device/rmweb): the lock is an
# atomic mkdir. If it exists, another instance owns xochitl + the overlay — bail WITHOUT
# touching either (no xochitl stop, no umount under a foreign instance).
if ! mkdir "$R/.lock" 2>/dev/null; then
  echo "[device] ERROR: $R/.lock exists — another rmweb instance is running" >&2
  echo "[device] (stale lock? remove $R/.lock if no rmweb is running)" >&2
  exit 1
fi
# WPE spawns helpers from the baked /usr/libexec/wpe-webkit-2.0 and / is read-only -> overlay it.
# Install the cleanup trap BEFORE mounting/stopping anything, so a failure in the setup block
# still restores xochitl + unmounts the overlay (the guard vars default to unset = no-op).
# DONE makes cleanup idempotent (EXIT re-runs after the TERM/INT/HUP traps).
DONE=
cleanup(){
  [ -n "$DONE" ] && return; DONE=1
  [ -n "${STOPPED:-}" ] && systemctl start xochitl
  [ -n "${MOUNTED:-}" ] && umount /usr/libexec 2>/dev/null   # umount only what WE mounted (flag)
  rmdir "$R/.lock" 2>/dev/null
}
trap cleanup EXIT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 129' HUP
# A hard-killed prior run leaves a stale overlay (its EXIT trap never ran) whose upper holds the OLD
# helpers — and the [ ! -e ] check below would then skip re-mounting and silently run stale binaries.
# Dropping it is safe only now that we hold the lock (no live instance can own that overlay).
umount /usr/libexec 2>/dev/null || true
if [ ! -e /usr/libexec/wpe-webkit-2.0 ]; then
  rm -rf "$R/ovl"; mkdir -p "$R/ovl/upper/wpe-webkit-2.0" "$R/ovl/work"
  cp -a "$R/libexec/wpe-webkit-2.0/." "$R/ovl/upper/wpe-webkit-2.0/"
  mount -t overlay overlay -o lowerdir=/usr/libexec,upperdir="$R/ovl/upper",workdir="$R/ovl/work" /usr/libexec && MOUNTED=1
fi

# Production runtime env — single source of truth (shared with the on-device launcher device/rmweb).
. "$R/rmweb-env.sh"

if [ "$MODE" = show ]; then
  # Stop xochitl only if it is actually running (device/rmweb discipline); STOPPED gates the restore.
  if systemctl is-active --quiet xochitl; then echo "[device] stopping xochitl"; systemctl stop xochitl && STOPPED=1; fi
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
  export RMWEB_DEBUG_FIND    # diagnostic: run an in-page find for this term once after 6 s
  export RMWEB_DEBUG_PROBE   # diagnostic: content tap probe at panel "x,y" once after 4 s
  export RMWEB_DEBUG_FORM    # diagnostic: tap probe + commit text into the focused field ("x,y,text")
  export RMWEB_DEBUG_SEARCH  # diagnostic: run the address-bar search for these words once after 4 s
  export RMWEB_NOJS          # diagnostic: disable JavaScript entirely (split JS vs CSS/network cost)
  export RMWEB_DEBUG_UITAP   # diagnostic: synthetic ROUTER tap at panel "x,y" once after 5 s (chrome/badge paths)
  export RMWEB_AUTOREFRESH_MS  # auto-refresh guard: min ms between same-URL auto-navigations (default 15000)
  export QT_LOGGING_RULES    # e.g. rmweb.engine.debug=true enables the qCDebug [t]/[perf] traces
  if [ "${SHOW_SECS:-180}" = "0" ]; then
    echo "[device] showing on e-ink until process exits (SHOW_SECS=0) ..."
    "$R/bin/rmweb-wpeqt" "$URL" >"$R/wpeqt.log" 2>&1 || true
    tail -n 40 "$R/wpeqt.log"
  else
    echo "[device] showing on e-ink for ${SHOW_SECS}s (set SHOW_SECS=0 for unlimited) ..."
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
    tail -n 40 "$R/wpeqt.log"
  fi
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
  "$R/bin/rmweb-wpeqt" "$URL" "$R/qt-out.png" 2>&1 | tail -n 30 || echo "[device] rc=$?"
  echo "[device] result: $(ls -l "$R/qt-out.png" 2>&1)"
fi
EOS

if [ "$MODE" != show ]; then
  echo "[host] copying device PNG -> build/qt-out-device.png"
  scp -q "$DUSER@$HOST:/home/root/rmweb/qt-out.png" build/qt-out-device.png 2>/dev/null \
    && file build/qt-out-device.png || { echo "no PNG produced"; exit 1; }
fi
