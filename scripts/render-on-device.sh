#!/usr/bin/env bash
set -euo pipefail
# Run the WPE headless render PROOF on the device (native glibc 2.39, software GL — the
# on-die GPU has no driver in the stock OS),
# write /home/root/rmweb/out.png, and copy it back to build/wpe-render-device.png.
# Requires scripts/bundle.sh to have deployed /home/root/rmweb first.
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"; DUSER="${REMARKABLE_USER:-${DEVICE_USER:-root}}"   # REMARKABLE_USER (.env) is canonical; DEVICE_USER = legacy fallback
OUT="${1:-build/wpe-render-device.png}"

ssh "$DUSER@$HOST" 'bash -s' <<'EOS'
set -e
R=/home/root/rmweb
# WPE spawns helpers from the BAKED /usr/libexec/wpe-webkit-2.0 (WEBKIT_EXEC_PATH is ignored in
# 2.48). / is mounted READ-ONLY, so we can't symlink into /usr/libexec. Overlay-mount it instead:
# lower = the real /usr/libexec (keeps dbus/sftp/fc-cache/...), upper = our wpe-webkit-2.0
# helpers. Fully reversible (umount), no rootfs write. WPE bakes this absolute path
# (WEBKIT_EXEC_PATH is ignored in 2.48); a clean -DCMAKE_INSTALL_PREFIX=/home/root/rmweb
# rebuild is the Phase-5 packaging fix.
OVL="$R/ovl"
if [ ! -e /usr/libexec/wpe-webkit-2.0 ]; then
  rm -rf "$OVL"; mkdir -p "$OVL/upper/wpe-webkit-2.0" "$OVL/work"
  cp -a "$R/libexec/wpe-webkit-2.0/." "$OVL/upper/wpe-webkit-2.0/"
  mount -t overlay overlay -o lowerdir=/usr/libexec,upperdir="$OVL/upper",workdir="$OVL/work" /usr/libexec && MOUNTED=1
fi
trap '[ -n "${MOUNTED:-}" ] && umount /usr/libexec 2>/dev/null' EXIT
echo "[device] helper path: $(ls -ld /usr/libexec/wpe-webkit-2.0 2>&1)"

export LD_LIBRARY_PATH="$R/lib"
export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless   # llvmpipe = multi-core+SIMD SW GL (bundle ships libLLVM); softpipe was ~64x slower
export LIBGL_DRIVERS_PATH="$R/lib/dri"
export __EGL_VENDOR_LIBRARY_DIRS="$R/share/glvnd/egl_vendor.d"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_INJECTED_BUNDLE_PATH="$R/lib/wpe-webkit-2.0/injected-bundle"
export GIO_EXTRA_MODULES="$R/lib/gio/modules"   # glib-networking OpenSSL TLS backend -> https:// works
export FONTCONFIG_PATH=/etc/fonts HOME=/home/root
echo "[device] system ttf fonts: $(find /usr/share/fonts -name '*.ttf' 2>/dev/null | wc -l)"
rm -f "$R/out.png"
"$R/bin/wpe_render" "$R/out.png" 2>&1 | tail -n 25 || echo "[device] wpe_render rc=$?"
echo "[device] result: $(ls -l "$R/out.png" 2>&1)"
EOS

echo "[host] copying device PNG -> $OUT"
if scp -q "$DUSER@$HOST:/home/root/rmweb/out.png" "$OUT" 2>/dev/null; then
  file "$OUT"
else
  echo "no device PNG produced"; exit 1
fi
