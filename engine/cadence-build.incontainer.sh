#!/bin/bash
# Runs INSIDE rmweb-sdk. Cross-compiles engine/wpe_cadence.c against the staged WPE WebKit into an
# aarch64 binary at build/wpe_cadence (to be run ON THE DEVICE, not here). Compile-only — no run.
set -e
. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux

STAGE=/work/build/stage
MESA=/work/build/stage-mesa
SR=$SDKTARGETSYSROOT
cp -a "$STAGE/usr/." "$SR/usr/" 2>/dev/null || true
cp -a "$MESA/usr/." "$SR/usr/" 2>/dev/null || true
export PKG_CONFIG_PATH="$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

CF="$(pkg-config --cflags wpe-webkit-2.0 wpe-platform-2.0 glib-2.0)"
LF="$(pkg-config --libs wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 gobject-2.0) -latomic"

echo "=== compiling wpe_cadence.c (native aarch64) ==="
$CC --sysroot=$SR -O2 -mcpu=cortex-a53+crc+crypto -rdynamic \
  /work/engine/wpe_cadence.c $CF $LF \
  -Wl,-rpath-link,"$SR/usr/lib" \
  -o /work/build/wpe_cadence 2>&1 | head -60
test -x /work/build/wpe_cadence && echo "BUILD OK" || { echo "BUILD FAILED"; exit 1; }
