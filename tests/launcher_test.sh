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
