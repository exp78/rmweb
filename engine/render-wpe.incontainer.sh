#!/bin/bash
# Runs INSIDE rmweb-sdk. Builds build/wpe_render.c against the freshly installed
# WPE WebKit (in build/stage) and runs it on the software GL stack to render a
# page to /work/build/wpe-render.png.
set -e
. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux

# A font so text actually paints: the SDK sysroot ships fonts.conf but ZERO fonts. The sysroot
# fonts.conf scans the ABSOLUTE /usr/share/fonts, which is exactly where this debian package
# installs DejaVuSans.ttf -> the (sysroot) fontconfig in the WebProcess then finds it.
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq fonts-dejavu-core >/dev/null 2>&1 \
  || echo "[render] WARN: DejaVu font install failed (text may render blank)"

STAGE=/work/build/stage
MESA=/work/build/stage-mesa
SR=$SDKTARGETSYSROOT

# Seed sysroot from both stages (idempotent) so headers/.pc/libs resolve.
cp -a "$STAGE/usr/." "$SR/usr/" 2>/dev/null || true
cp -a "$MESA/usr/." "$SR/usr/" 2>/dev/null || true

export PKG_CONFIG_PATH="$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

echo "=== pkg-config sanity ==="
pkg-config --exists wpe-webkit-2.0 && echo "wpe-webkit-2.0: $(pkg-config --modversion wpe-webkit-2.0)" || { echo "MISSING wpe-webkit-2.0 .pc"; ls $SR/usr/lib/pkgconfig | grep -i wpe; }
pkg-config --exists wpe-platform-2.0 && echo "wpe-platform-2.0: $(pkg-config --modversion wpe-platform-2.0)" || echo "MISSING wpe-platform-2.0"

# pkg-config gives the correct -I:
#   wpe-webkit-2.0  -> include/wpe-webkit-2.0          (<wpe/webkit.h>)
#   wpe-platform-2.0-> include/wpe-webkit-2.0/wpe-platform (<wpe/wpe-platform.h>, <wpe/headless/wpe-headless.h>)
CF="$(pkg-config --cflags wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 libpng)"
LF="$(pkg-config --libs wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 gobject-2.0 libpng) -latomic"

echo "=== compiling wpe_render.c (native aarch64) ==="
$CC --sysroot=$SR -O2 -mcpu=cortex-a53+crc+crypto \
  /work/engine/wpe_render.c $CF $LF \
  -Wl,-rpath-link,"$SR/usr/lib" \
  -o /work/build/wpe_render 2>&1 | head -60
test -x /work/build/wpe_render && echo "BUILD OK" || { echo "BUILD FAILED"; exit 1; }

echo "=== preparing software-GL runtime ==="
# WPE spawns subprocesses (Web/GPU/Network) whose ELF interp is /lib/ld-linux-aarch64.so.1.
# The container ld.so is glibc 2.36; loading our 2.39 sysroot libc under it fails with
# "undefined symbol __tunable_is_initialized, version GLIBC_PRIVATE". Repoint the container
# loader to the sysroot 2.39 loader so EVERY aarch64 binary (our target ones AND plain
# container tools) runs under the matched/newer loader (a newer ld.so runs older binaries
# fine). The container is --rm, so this throwaway change is harmless.
ln -sf "$SR/lib/ld-linux-aarch64.so.1" /lib/ld-linux-aarch64.so.1
LOADER="$SR/lib/ld-linux-aarch64.so.1"
LIBS="$STAGE/usr/lib:$MESA/usr/lib:$SR/usr/lib:$SR/lib"

# WPE bakes its install prefix (/usr) into the binary: it spawns helpers from the ABSOLUTE
# /usr/libexec/wpe-webkit-2.0/ and loads resources/injected-bundle from /usr/{lib,share}/
# wpe-webkit-2.0/ (WEBKIT_EXEC_PATH is NOT honored for this in 2.48). We installed under
# $STAGE/usr, so symlink those subtrees into the container's real /usr (container is --rm).
mkdir -p /usr/libexec /usr/lib /usr/share
ln -sfn "$STAGE/usr/libexec/wpe-webkit-2.0" /usr/libexec/wpe-webkit-2.0
ln -sfn "$STAGE/usr/lib/wpe-webkit-2.0"     /usr/lib/wpe-webkit-2.0
ln -sfn "$STAGE/usr/share/wpe-webkit-2.0"   /usr/share/wpe-webkit-2.0

EXEC="$STAGE/usr/libexec/wpe-webkit-2.0"   # WEBKIT_EXEC_PATH is ignored in 2.48; the /usr symlinks above are what work. Set anyway (harmless).
echo "[render] webkit helpers:"; ls "$EXEC" 2>/dev/null

rm -f /work/build/wpe-render.png
echo "=== running render on software GL (softpipe, surfaceless) ==="
# Scope the sysroot LD_LIBRARY_PATH + GL env to the render (and its inherited subprocesses)
# ONLY, via env, so plain container commands keep their default libc.
env \
  LD_LIBRARY_PATH="$LIBS" \
  LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless GALLIUM_DRIVER=softpipe \
  LIBGL_DRIVERS_PATH="$MESA/usr/lib/dri" \
  __EGL_VENDOR_LIBRARY_DIRS="$MESA/usr/share/glvnd/egl_vendor.d" \
  WEBKIT_EXEC_PATH="$EXEC" \
  WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 \
  WEBKIT_INJECTED_BUNDLE_PATH="$STAGE/usr/lib/wpe-webkit-2.0/injected-bundle" \
  FONTCONFIG_PATH="$SR/etc/fonts" HOME=/tmp \
  "$LOADER" --library-path "$LIBS" /work/build/wpe_render 2>&1 | tail -40 || true

echo "=== result ==="
# Use the bash [ builtin (not a forked binary): the loader repoint above can segfault plain
# container commands run outside the scoped env. Real size is verified on the host.
[ -s /work/build/wpe-render.png ] && echo "PNG WRITTEN" || echo "NO PNG"
