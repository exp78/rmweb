#!/usr/bin/env bash
set -euo pipefail
# Copy a built binary to the device under /home/root/rmweb/bin/ and run it over SSH.
# Usage: scripts/deploy.sh <local-binary>
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env
HOST="${REMARKABLE_HOST:-10.11.99.1}"
DEVICE_USER="${REMARKABLE_USER:-${DEVICE_USER:-root}}"   # REMARKABLE_USER (.env) is canonical; DEVICE_USER = legacy fallback
BIN="${1:?usage: deploy.sh <local-binary>}"
NAME="$(basename "$BIN")"
REMOTE="/home/root/rmweb/bin/$NAME"
ssh "$DEVICE_USER@$HOST" 'mkdir -p /home/root/rmweb/bin'
scp "$BIN" "$DEVICE_USER@$HOST:$REMOTE"
echo "=== running $REMOTE on device ==="
ssh "$DEVICE_USER@$HOST" "exec $(printf '%q' "$REMOTE")"   # %q: an apostrophe in the name must not break out of quoting (root shell)
