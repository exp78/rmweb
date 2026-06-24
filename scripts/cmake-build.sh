#!/usr/bin/env bash
set -euo pipefail
# Configure + build a CMake project inside the SDK container (cross to aarch64).
# CMake auto-uses $CMAKE_TOOLCHAIN_FILE from the sourced SDK env (Qt6 cross + cortex-a53).
# Usage: scripts/cmake-build.sh <source-dir> [build-name]
cd "$(dirname "$0")/.."
SRC="${1:?usage: cmake-build.sh <source-dir> [build-name]}"
NAME="${2:-$(basename "$SRC")}"
docker run --rm -v "$PWD":/work -w /work rmweb-sdk bash -lc "
  . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux &&
  cmake -S '$SRC' -B 'build/$NAME' -G Ninja -DCMAKE_BUILD_TYPE=Release &&
  cmake --build 'build/$NAME' -j\$(nproc)
"
