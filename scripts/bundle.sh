#!/usr/bin/env bash
set -euo pipefail
# Assemble the WPE runtime bundle and deploy it to /home/root/rmweb on the Paper Pro.
# Ships our built .so (+ sqlite3/webp pulled from the SDK sysroot, which the device runtime
# lacks), the Mesa software-GL drivers, the WPE subprocess helpers, WebKit resources +
# injected-bundle, and the wpe_render proof binary. The other ~24 deps (glib, icu, cairo,
# freetype, harfbuzz, openssl, libxml2, png/jpeg, ...) are reused from the device.
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"; DUSER="${DEVICE_USER:-root}"
S=build/stage/usr; M=build/stage-mesa/usr; B=build/bundle

rm -rf "$B"; mkdir -p "$B/lib/dri" "$B/libexec" "$B/share/glvnd/egl_vendor.d" "$B/bin"
cp -a "$S"/lib/*.so*                "$B/lib/"
cp -a "$M"/lib/*.so*                "$B/lib/"
cp -a "$M"/lib/dri/*.so             "$B/lib/dri/"                    2>/dev/null || true
cp -a "$M"/share/glvnd/egl_vendor.d/. "$B/share/glvnd/egl_vendor.d/" 2>/dev/null || true
cp -a "$S"/libexec/wpe-webkit-2.0   "$B/libexec/"
cp -a "$S"/lib/wpe-webkit-2.0       "$B/lib/"                        2>/dev/null || true   # injected-bundle
cp -a "$S"/share/wpe-webkit-2.0     "$B/share/"                      2>/dev/null || true   # resources
cp -a build/wpe_render              "$B/bin/"

echo "[bundle] local size:"; du -sh "$B"
echo "[bundle] deploying to $DUSER@$HOST:/home/root/rmweb ..."
tar -C "$B" -cf - . | ssh "$DUSER@$HOST" 'mkdir -p /home/root/rmweb && tar -C /home/root/rmweb -xf -'
ssh "$DUSER@$HOST" 'echo "[device] /home/root/rmweb:"; du -sh /home/root/rmweb; ls /home/root/rmweb'
