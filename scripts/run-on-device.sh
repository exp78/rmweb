#!/usr/bin/env bash
set -euo pipefail
# Deploy a binary to the device, stop xochitl, run it fullscreen via the epaper QPA,
# then ALWAYS restart xochitl (even if the app crashes).
# Usage: scripts/run-on-device.sh <local-binary> [app-args...]
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"
DEVICE_USER="${REMARKABLE_USER:-${DEVICE_USER:-root}}"   # REMARKABLE_USER (.env) is canonical; DEVICE_USER = legacy fallback
BIN="${1:?usage: run-on-device.sh <local-binary> [app-args...]}"; shift || true
NAME="$(basename "$BIN")"
REMOTE="/home/root/rmweb/bin/$NAME"
REMOTE_ARGS="$(printf '%q ' "$@")"   # shell-escape app args for the remote shell

ssh "$DEVICE_USER@$HOST" 'mkdir -p /home/root/rmweb/bin'
scp "$BIN" "$DEVICE_USER@$HOST:$REMOTE"
ssh "$DEVICE_USER@$HOST" "
  set -e
  cleanup(){
    trap '' TERM INT HUP   # a repeated signal mid-cleanup must not interrupt the xochitl restore
    [ -n \"\${DONE:-}\" ] && return; DONE=1
    [ -n \"\${APP:-}\" ] && kill \"\$APP\" 2>/dev/null
    # rmweb's TERM handler drains the in-flight e-ink present + flushes the profile first (~2.7 s) —
    # give it that window (same bounded wait as device/rmweb); the kill -9 sweep is for what survives.
    i=0; while [ -n \"\${APP:-}\" ] && kill -0 \"\$APP\" 2>/dev/null && [ \"\$i\" -lt 6 ]; do sleep 0.5; i=\$((i+1)); done
    for n in rmweb-wpeqt WPEWebProcess WPENetworkProc WPEGPUProcess; do
      for p in \$(pgrep \"\$n\" 2>/dev/null); do kill -9 \"\$p\" 2>/dev/null; done
    done
    # StartLimitIntervalSec/Burst: repeated stop/start cycles trip an emergency reboot — clear the counter.
    echo \"[device] restarting xochitl…\"; systemctl reset-failed xochitl 2>/dev/null; systemctl start xochitl
  }
  trap cleanup EXIT
  trap 'cleanup; exit 143' TERM
  trap 'cleanup; exit 130' INT
  echo '[device] stopping xochitl…'
  systemctl stop xochitl
  echo '[device] running $NAME via epaper QPA…'
  QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper '$REMOTE' $REMOTE_ARGS &
  APP=\$!
  wait \$APP || echo \"[device] app exited rc=\$?\"
"
