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
REMOTE_ARGS="$(printf '%q ' "$@")"   # shell-escape app args for the remote shell

ssh "$DEVICE_USER@$HOST" 'mkdir -p /home/root/rmweb/bin'
scp "$BIN" "$DEVICE_USER@$HOST:$REMOTE"
ssh "$DEVICE_USER@$HOST" "
  set -e
  trap 'echo \"[device] restarting xochitl…\"; systemctl start xochitl' EXIT   # restore BEFORE stopping
  echo '[device] stopping xochitl…'
  systemctl stop xochitl
  echo '[device] running $NAME via epaper QPA…'
  QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper '$REMOTE' $REMOTE_ARGS || echo \"[device] app exited rc=\$?\"
"
