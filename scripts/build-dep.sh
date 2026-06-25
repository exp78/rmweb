#!/usr/bin/env bash
set -euo pipefail
# Cross-build one dependency into the rmweb staging prefix (build/stage/usr) using the SDK
# container. The source is fetched + extracted on the HOST (the container has no curl), then
# built inside the container. Each run re-seeds the SDK sysroot from build/stage so this build
# finds prior deps. Usage:
#   scripts/build-dep.sh <name> <tarball-url> <meson|cmake|autotools> [config args...]
cd "$(dirname "$0")/.."
NAME="${1:?name}"; URL="${2:?tarball-url}"; SYS="${3:?meson|cmake|autotools}"; shift 3 || true

SRC="build/src/$NAME"
echo "[build-dep] fetching $NAME (host) from $URL"
rm -rf "$SRC"; mkdir -p "$SRC"
curl -fL "$URL" -o "$SRC.tar"
tar xf "$SRC.tar" -C "$SRC" --strip-components=1
rm -f "$SRC.tar"

docker run --rm -v "$PWD":/work -w /work \
  -e DEP_NAME="$NAME" -e DEP_SYS="$SYS" -e DEP_ARGS="$*" \
  rmweb-sdk bash -lc '
  set -e
  . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
  STAGE=/work/build/stage
  mkdir -p "$STAGE/usr"
  cp -a "$STAGE/usr/." "$SDKTARGETSYSROOT/usr/" 2>/dev/null || true   # seed prior deps
  cd "/work/build/src/$DEP_NAME"
  echo "[build-dep] building $DEP_NAME ($DEP_SYS) args: $DEP_ARGS"
  case "$DEP_SYS" in
    meson)     meson setup _b --prefix=/usr --buildtype=release $DEP_ARGS && ninja -C _b && DESTDIR="$STAGE" ninja -C _b install ;;
    cmake)     cmake -S . -B _b -GNinja -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release $DEP_ARGS && ninja -C _b && DESTDIR="$STAGE" ninja -C _b install ;;
    autotools) ./configure --prefix=/usr --host=aarch64-remarkable-linux $DEP_ARGS && make -j"$(nproc)" && make install DESTDIR="$STAGE" ;;
    *) echo "unknown build system: $DEP_SYS" >&2; exit 2 ;;
  esac
  echo "[build-dep] OK $DEP_NAME"
'
