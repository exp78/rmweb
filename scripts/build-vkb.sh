#!/usr/bin/env bash
set -euo pipefail
# Cross-build Qt Virtual Keyboard 6.8.2 for the Paper Pro -> build/stage-vkb/usr (QML module +
# input-context plugin). Source is fetched on the HOST (the rmweb-sdk container has no curl); the
# actual build runs in-container via engine/qtvirtualkeyboard.incontainer.sh on the rmweb-build volume.
cd "$(dirname "$0")/.."
REPO="$PWD"; IMG=rmweb-sdk
VER=6.8.2
SRC="build/src/qtvirtualkeyboard-$VER"

if [ ! -d "$SRC" ] || [ -z "$(ls -A "$SRC" 2>/dev/null)" ]; then
  echo "[fetch] qtvirtualkeyboard $VER (host)"
  mkdir -p "$SRC"
  tmp="$(mktemp "${TMPDIR:-/tmp}/_vkb.XXXXXX.tar.xz")"
  curl -fL "https://download.qt.io/archive/qt/6.8/$VER/submodules/qtvirtualkeyboard-everywhere-src-$VER.tar.xz" -o "$tmp"
  tar xf "$tmp" -C "$SRC" --strip-components=1 || { rm -rf "$SRC"; rm -f "$tmp"; exit 1; }
  rm -f "$tmp"
fi
echo "[src] $(ls "$SRC" | head -n 6 | tr '\n' ' ')"

docker run --rm -v "$REPO":/work -v rmweb-build:/build -w /work "$IMG" bash /work/engine/qtvirtualkeyboard.incontainer.sh
