#!/bin/bash
# Runs INSIDE rmweb-sdk. Cross-build Qt Virtual Keyboard 6.8.2 against the SDK-sysroot Qt 6.8.2
# (rmweb itself links the device's system Qt, which is newer; same-major plugins built with an
# older minor load fine) using the SDK's qt-cmake — the wrapper handles host tools (moc/qmltyperegistrar/qmlcachegen
# from the NATIVE sysroot, which run natively in this aarch64 container) vs the cross-compiled target libs.
# Builds on the fast case-sensitive /build volume (resumable). Staged to build/stage-vkb/usr:
#   * the QML module      .../qml/QtQuick/VirtualKeyboard/**
#   * the input-context   .../plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so
# Source is fetched on the HOST (container has no curl) into build/src/qtvirtualkeyboard-6.8.2.
set -e
set -o pipefail   # the configure/build/install pipes below must not mask a build failure
. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
SRC=/work/build/src/qtvirtualkeyboard-6.8.2
BLD=/build/vkb
STAGE=/work/build/stage-vkb
QTCMAKE="$OECORE_NATIVE_SYSROOT/usr/bin/qt-cmake"
echo "### qt-cmake=$QTCMAKE"; "$QTCMAKE" --version 2>&1 | head -n 2 || true
echo "### QT_HOST_PATH=${QT_HOST_PATH:-<unset>}  native=$OECORE_NATIVE_SYSROOT"

echo "### STAGE 1: configure ###"
# Hand-held layout only (no handwriting/T9 deps); disable examples/tests to keep it lean.
"$QTCMAKE" -S "$SRC" -B "$BLD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF \
  2>&1 | tail -n 40

echo "### STAGE 2: build ###"
cmake --build "$BLD" 2>&1 | tail -n 30

echo "### STAGE 3: install -> $STAGE ###"
rm -rf "$STAGE"; DESTDIR="$STAGE" cmake --install "$BLD" 2>&1 | tail -n 15

echo "### staged VKB artifacts ###"
find "$STAGE" \( -name 'libqtvirtualkeyboardplugin.so' -o \( -name 'qmldir' -path '*VirtualKeyboard*' \) \) 2>/dev/null | head -n 10
echo "### qml + plugin trees ###"
find "$STAGE" -type d -path '*VirtualKeyboard*' 2>/dev/null | head -n 5
find "$STAGE" -name '*.so' -path '*platforminputcontexts*' 2>/dev/null | head
