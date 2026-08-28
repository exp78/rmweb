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

echo "case 4: lock contention with a LIVE rmweb-wpeqt refuses to start (no xochitl touch)"
setup; mkdir "$R/.lock"
make_stub pgrep 'echo 999999; exit 0'   # a live process holds the lock (high PID: kill must not hit a real proc)
APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER"; rc=$?
[ "$rc" = 1 ] || { echo "  FAIL: expected rc=1, got $rc"; fails=$((fails+1)); }
nowant "systemctl stop xochitl"; nowant "systemctl start xochitl"
[ -d "$R/.lock" ] || { echo "  FAIL: pre-existing lock was removed"; fails=$((fails+1)); }

echo "case 5: TERM mid-run restores xochitl"
setup; APP_MODE=hang PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER" &
LPID=$!; sleep 1; kill -TERM "$LPID" 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do grep -q "systemctl start xochitl" "$STUBLOG" && break; sleep 0.5; done
wait "$LPID" 2>/dev/null
want "systemctl start xochitl"

echo "case 6: SIGHUP mid-run restores xochitl and releases the lock"
setup; APP_MODE=hang PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER" &
LPID=$!; sleep 1; kill -HUP "$LPID" 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do grep -q "systemctl start xochitl" "$STUBLOG" && break; sleep 0.5; done
wait "$LPID" 2>/dev/null
want "systemctl start xochitl"
[ -d "$R/.lock" ] && { echo "  FAIL: lock not released after HUP"; fails=$((fails+1)); }

echo "case 7: lingering pids delay but do not skip the xochitl restart"
setup
# pgrep reports a live pid for the first two calls, then nothing: cleanup's wait loop must
# ride it out and still reach reset-failed + start.
make_stub pgrep 'c=$(cat "'"$STUBLOG"'.n" 2>/dev/null || echo 0); c=$((c+1)); echo "$c" > "'"$STUBLOG"'.n"; [ "$c" -le 2 ] && { echo 999999; exit 0; }; exit 1'
APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER"
want "systemctl reset-failed xochitl"; want "systemctl start xochitl"

echo "case 8: oversized rmweb.log is rotated before launch"
setup; dd if=/dev/zero of="$R/rmweb.log" bs=1048576 count=6 2>/dev/null
APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER"
logsize=$(wc -c < "$R/rmweb.log" | tr -dc '0-9')
[ "$logsize" -lt 2097152 ] || { echo "  FAIL: log not rotated (size $logsize)"; fails=$((fails+1)); }
grep -q "log rotated" "$R/rmweb.log" || { echo "  FAIL: no rotation note in log"; fails=$((fails+1)); }

echo "case 9: stale lock (no live rmweb-wpeqt) is taken over"
setup; mkdir "$R/.lock"   # default pgrep stub exits 1 -> no live process -> stale
out=$(APP_MODE=clean PATH="$STUBS:$PATH" RMWEB_ROOT="$R" XOCHITL_ACTIVE=1 sh "$LAUNCHER" 2>&1); rc=$?
[ "$rc" = 0 ] || { echo "  FAIL: expected rc=0, got $rc"; fails=$((fails+1)); }
echo "$out" | grep -q "stale" || { echo "  FAIL: expected a stale-lock notice"; fails=$((fails+1)); }
want "systemctl stop xochitl"; want "systemctl start xochitl"
[ -d "$R/.lock" ] && { echo "  FAIL: lock not released"; fails=$((fails+1)); }

if [ "$fails" = 0 ]; then echo "launcher_test: OK"; else echo "launcher_test: $fails FAIL"; exit 1; fi
