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

rm -rf "$B"; mkdir -p "$B/lib/dri" "$B/lib/gio/modules" "$B/libexec" "$B/share/glvnd/egl_vendor.d" "$B/bin"
cp -a "$S"/lib/*.so*                "$B/lib/"
cp -a "$M"/lib/*.so*                "$B/lib/"
cp -a "$S"/lib/gio/modules/*.so     "$B/lib/gio/modules/"            2>/dev/null || true   # glib-networking (TLS)
cp -a "$M"/lib/dri/*.so             "$B/lib/dri/"                    2>/dev/null || true
cp -a "$M"/share/glvnd/egl_vendor.d/. "$B/share/glvnd/egl_vendor.d/" 2>/dev/null || true
# llvmpipe: multi-core + SIMD-JIT software rasterizer — ~64x faster page compositing than softpipe
# (fixes the ~6 s page turn; see the six-second-render memory). Replace the softpipe swrast driver with
# the llvm-enabled build, and bundle libLLVM + only the deps the device lacks (libz3/tinfo/edit/bsd/md).
# libxml2/ICU are NOT bundled (device provides libxml2). Run with GALLIUM_DRIVER=llvmpipe.
if [ -f build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so ]; then
  cp -a build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so "$B/lib/dri/swrast_dri.so"
  # Copy each lib individually so a missing one WARNs loudly instead of aborting the bundle under `set -e`
  # (and so an incomplete set is obvious rather than a silent runtime dlopen failure on the device).
  for lib in libLLVM-16.so.1 libz3.so.4 libtinfo.so.6 libedit.so.2 libbsd.so.0 libmd.so.0; do
    if [ -f "build/llvm-bundle/$lib" ]; then cp -aL "build/llvm-bundle/$lib" "$B/lib/"
    else echo "[bundle] WARN: build/llvm-bundle/$lib MISSING — llvmpipe will fail to dlopen on device"; fi
  done
else
  echo "[bundle] WARN: no llvmpipe build (build/stage-mesa-llvm) — shipping softpipe (slow). Run engine/mesa-llvmpipe.incontainer.sh"
fi
cp -a "$S"/libexec/wpe-webkit-2.0   "$B/libexec/"
cp -a "$S"/lib/wpe-webkit-2.0       "$B/lib/"                        2>/dev/null || true   # injected-bundle
cp -a "$S"/share/wpe-webkit-2.0     "$B/share/"                      2>/dev/null || true   # resources
cp -a build/wpe_render              "$B/bin/"

echo "[bundle] local size:"; du -sh "$B"
echo "[bundle] deploying to $DUSER@$HOST:/home/root/rmweb ..."
tar -C "$B" -cf - . | ssh "$DUSER@$HOST" 'mkdir -p /home/root/rmweb && tar -C /home/root/rmweb -xf -'
ssh "$DUSER@$HOST" 'echo "[device] /home/root/rmweb:"; du -sh /home/root/rmweb; ls /home/root/rmweb'
