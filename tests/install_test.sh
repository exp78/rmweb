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
[ "$(cat "$R/VERSION")" = "0.8.0" ] || { echo "  FAIL: VERSION content wrong (expected 0.8.0)"; fails=$((fails+1)); }
[ -x "$R/rmweb" ]   || { echo "  FAIL: launcher not executable"; fails=$((fails+1)); }

if [ "$fails" = 0 ]; then echo "install_test: OK"; else echo "install_test: $fails FAIL"; exit 1; fi
