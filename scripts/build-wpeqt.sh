#!/usr/bin/env bash
set -euo pipefail
# Cross-build the Qt6 + WPE integration app (engine/wpeqt) -> build/rmweb-wpeqt.
# Qt6 comes from the device sysroot via the OE CMake toolchain (as the Phase 1 display spike);
# WPE WebKit + GLib come from our staged prefix, seeded into the sysroot + found by pkg-config.
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/work -w /work rmweb-sdk bash -lc '
  set -e
  . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
  SR=$SDKTARGETSYSROOT
  cp -a /work/build/stage/usr/.      "$SR/usr/"
  cp -a /work/build/stage-mesa/usr/. "$SR/usr/"
  rm -rf /work/build/wpeqt
  cmake -S /work/engine/wpeqt -B /work/build/wpeqt -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build /work/build/wpeqt -j"$(nproc)"
  cp -f /work/build/wpeqt/rmweb-wpeqt /work/build/rmweb-wpeqt
  echo "[wpeqt] built:"; ls -l /work/build/rmweb-wpeqt
'
