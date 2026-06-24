#!/usr/bin/env bash
set -euo pipefail
# Copy a built binary to the device under /home/root/rmweb/bin/ and run it over SSH.
# Usage: scripts/deploy.sh <local-binary>
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env
HOST="${REMARKABLE_HOST:-10.11.99.1}"
USER="${REMARKABLE_USER:-root}"
BIN="${1:?usage: deploy.sh <local-binary>}"
NAME="$(basename "$BIN")"
ssh "$USER@$HOST" 'mkdir -p /home/root/rmweb/bin'
scp "$BIN" "$USER@$HOST:/home/root/rmweb/bin/$NAME"
echo "=== running /home/root/rmweb/bin/$NAME on device ==="
ssh "$USER@$HOST" "/home/root/rmweb/bin/$NAME"
