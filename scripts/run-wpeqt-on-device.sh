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

ssh "$DUSER@$HOST" "MODE='$MODE' URL='$URL' SHOW_SECS='${SHOW_SECS:-40}' bash -s" <<'EOS'
set -e
R=/home/root/rmweb
# WPE spawns helpers from the baked /usr/libexec/wpe-webkit-2.0 and / is read-only -> overlay it.
# Install the cleanup trap BEFORE mounting/stopping anything, so a failure in the setup block
# still restores xochitl + unmounts the overlay (the guard vars default to unset = no-op).
cleanup(){ [ -n "${STOPPED:-}" ] && systemctl start xochitl; [ -n "${MOUNTED:-}" ] && umount /usr/libexec 2>/dev/null; }
trap cleanup EXIT
if [ ! -e /usr/libexec/wpe-webkit-2.0 ]; then
  rm -rf "$R/ovl"; mkdir -p "$R/ovl/upper/wpe-webkit-2.0" "$R/ovl/work"
  cp -a "$R/libexec/wpe-webkit-2.0/." "$R/ovl/upper/wpe-webkit-2.0/"
  mount -t overlay overlay -o lowerdir=/usr/libexec,upperdir="$R/ovl/upper",workdir="$R/ovl/work" /usr/libexec && MOUNTED=1
fi

export LD_LIBRARY_PATH="$R/lib"
export GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless
export LIBGL_DRIVERS_PATH="$R/lib/dri" __EGL_VENDOR_LIBRARY_DIRS="$R/share/glvnd/egl_vendor.d"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_INJECTED_BUNDLE_PATH="$R/lib/wpe-webkit-2.0/injected-bundle"
export FONTCONFIG_PATH=/etc/fonts HOME=/home/root

if [ "$MODE" = show ]; then
  echo "[device] stopping xochitl"; systemctl stop xochitl && STOPPED=1
  export QT_QPA_PLATFORM=epaper QT_QUICK_BACKEND=epaper
  echo "[device] showing on e-ink for ${SHOW_SECS}s ..."
  # BusyBox here has no `timeout`; run in the background and kill after SHOW_SECS.
  "$R/bin/rmweb-wpeqt" "$URL" >/tmp/wpeqt.log 2>&1 &
  APP=$!
  sleep "$SHOW_SECS"
  kill "$APP" 2>/dev/null || true
  wait "$APP" 2>/dev/null || true
  tail -n 30 /tmp/wpeqt.log
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
