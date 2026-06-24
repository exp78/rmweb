#!/usr/bin/env bash
set -euo pipefail
# Deploy a binary to the device, stop xochitl, run it fullscreen via the epaper QPA,
# then ALWAYS restart xochitl (even if the app crashes).
# Usage: scripts/run-on-device.sh <local-binary> [app-args...]
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env
HOST="${REMARKABLE_HOST:-10.11.99.1}"
DEVICE_USER="${REMARKABLE_USER:-root}"
BIN="${1:?usage: run-on-device.sh <local-binary> [app-args...]}"; shift || true
NAME="$(basename "$BIN")"
REMOTE="/home/root/rmweb/bin/$NAME"
ARGS="$*"

ssh "$DEVICE_USER@$HOST" 'mkdir -p /home/root/rmweb/bin'
scp "$BIN" "$DEVICE_USER@$HOST:$REMOTE"
ssh "$DEVICE_USER@$HOST" "
  set -e
  echo '[device] stopping xochitl…'
  systemctl stop xochitl
  trap 'echo \"[device] restarting xochitl…\"; systemctl start xochitl' EXIT
  echo '[device] running $NAME via epaper QPA…'
  QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper '$REMOTE' $ARGS || echo \"[device] app exited rc=\$?\"
"
