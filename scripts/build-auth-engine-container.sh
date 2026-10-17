#!/bin/bash
# Runs only in the dedicated offline SDK container.
set -euo pipefail
export LC_ALL=C.UTF-8
jobs=${1:?worker count required}
mkdir -p /work/logs
# Earlier downstream markers require a fresh volume; never relabel cached objects.
if [[ -f /build/.purpose ]]; then
    [[ $(cat /build/.purpose) == rmweb-auth-webauthn-engine-v1 ]] || {
        echo 'Unrecognized build volume; use a fresh dedicated rmweb build volume' >&2
        exit 1
    }
else
    [[ -z $(ls -A /build) ]]
    printf '%s\n' rmweb-auth-webauthn-engine-v1 >/build/.purpose
fi
[[ ! -L /build/.engine-build.lock ]]
[[ ! -e /build/.engine-build.lock || -f /build/.engine-build.lock ]]
exec 9>>/build/.engine-build.lock
flock -n 9
for directory in dev configure-webauthn-on; do
    [[ ! -L /build/$directory ]]
    [[ ! -e /build/$directory || -d /build/$directory ]]
done
python3 /src/scripts/build-auth-engine.py --container-prepare
dpkg -i /work/host-packages/*.deb >/work/logs/host-install.log 2>&1
# shellcheck source=/dev/null
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
export LC_ALL=C.UTF-8
test "$(qmake -query QT_VERSION)" = 6.10.3
SR=$SDKTARGETSYSROOT

# Only development declarations are taken from these byte-pinned Ubuntu24
# packages. No Ubuntu target libraries or executables enter the SDK sysroot.
for package in /work/packages/*.deb; do
    dpkg-deb -x "$package" /build/dev
done
cp -a /work/headers/stage/usr/. "$SR/usr/"
cp -a /build/dev/usr/include/. "$SR/usr/include/"
# Debian keeps this generated ARM64 header in its multiarch include directory;
# the SDK uses the ordinary /usr/include/libxslt location.
cp -p /build/dev/usr/include/aarch64-linux-gnu/libxslt/xsltconfig.h "$SR/usr/include/libxslt/"
cp -a /build/runtime/lib/. "$SR/usr/lib/"
cp -p /work/inputs/gbm-24.0.9.h "$SR/usr/include/gbm.h"
# Keep target pkg-config paths under /usr/lib, not Debian multiarch paths.
for name in epoxy harfbuzz-icu libtasn1 libxslt libexslt egl glesv2; do
    file="/build/dev/usr/lib/aarch64-linux-gnu/pkgconfig/$name.pc"
    test -f "$file"
    sed 's@/lib/aarch64-linux-gnu@/lib@g' "$file" >"$SR/usr/lib/pkgconfig/$name.pc"
done
cat >"$SR/usr/lib/pkgconfig/xkbcommon.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include
Name: xkbcommon
Description: Pinned rmweb1.7.0 runtime development overlay
Version: 1.7.0
Libs: -L${libdir} -lxkbcommon
Cflags: -I${includedir}
PC

export PKG_CONFIG_PATH="$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export PKG_CONFIG_SYSROOT_DIR="$SR"
for package in glib-2.0 gio-2.0 harfbuzz-icu epoxy libtasn1 xkbcommon wpe-1.0 libsoup-3.0 libxslt egl glesv2; do
    printf '%s %s\n' "$package" "$(pkg-config --modversion "$package")"
done | tee /work/logs/dependency-versions.txt

unset CMAKE_TOOLCHAIN_FILE CC CXX CPP LD AR NM STRIP RANLIB OBJCOPY OBJDUMP READELF
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS ASFLAGS OECORE_TUNE_CCARGS CONFIG_SITE
CPUFLAGS="-mcpu=cortex-a53+crc+crypto -mbranch-protection=standard"
cmake -S /build/wpewebkit-2.48.5 -B /build/configure-webauthn-on -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-remarkable-linux-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-remarkable-linux-g++ \
    -DCMAKE_C_FLAGS="$CPUFLAGS --sysroot=$SR" \
    -DCMAKE_CXX_FLAGS="$CPUFLAGS --sysroot=$SR" \
    -DCMAKE_EXE_LINKER_FLAGS="-latomic -Wl,-rpath-link,$SR/usr/lib" \
    -DCMAKE_SHARED_LINKER_FLAGS="-latomic -Wl,-rpath-link,$SR/usr/lib" \
    -DCMAKE_MODULE_LINKER_FLAGS="-latomic -Wl,-rpath-link,$SR/usr/lib" \
    -DCMAKE_SYSROOT="$SR" -DCMAKE_FIND_ROOT_PATH="$SR" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_JOB_POOLS=link_pool=1 -DCMAKE_JOB_POOL_LINK=link_pool \
    -DPORT=WPE -DUSE_SKIA=ON -DENABLE_WEB_AUTHN=ON \
    -DUSE_GSTREAMER=OFF -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_SOURCE=OFF \
    -DENABLE_MEDIA_STREAM=OFF -DENABLE_WEB_CODECS=OFF -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DENABLE_WEBGL=OFF -DENABLE_WEBXR=OFF -DENABLE_WEB_RTC=OFF \
    -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=OFF -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    -DENABLE_INTROSPECTION=OFF -DENABLE_DOCUMENTATION=OFF \
    -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LIBHYPHEN=OFF -DENABLE_JOURNALD_LOG=OFF \
    -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_HEADLESS=ON \
    -DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF \
    -DENABLE_WPE_QT_API=OFF -DUSE_ATK=OFF -DUSE_WOFF2=OFF \
    -DUSE_LIBBACKTRACE=OFF -DUSE_SYSPROF_CAPTURE=OFF \
    -DENABLE_MINIBROWSER=OFF -DENABLE_COG=OFF -DENABLE_API_TESTS=OFF -DENABLE_WEBDRIVER=OFF

# Require the real provider/API objects first, including their generated support.
mapfile -t focused < <(python3 - <<'PYFOCUS'
from pathlib import Path
base = Path('/build/configure-webauthn-on')
for name in ('UIProcess/WebAuthentication/wpe/WebAuthenticatorCoordinatorProxyWPE.cpp',
             'UIProcess/API/glib/WebKitWebAuthenticationRequest.cpp',
             'UIProcess/API/glib/WebKitUIClient.cpp', 'UIProcess/API/glib/WebKitWebView.cpp'):
    print('Source/WebKit/CMakeFiles/WebKit.dir/' + name + '.o')
for source in sorted((base / 'DerivedSources/WebKit/unified-sources').glob('*.cpp')):
    if any(name in source.read_text() for name in ('UIProcess/WebPageProxy.cpp',
            'UIProcess/WebsiteData/WebsiteDataStore.cpp', 'UIProcess/Automation/WebAutomationSession.cpp')):
        print('Source/WebKit/CMakeFiles/WebKit.dir/__/__/DerivedSources/WebKit/unified-sources/' + source.name + '.o')
PYFOCUS
)
ninja -C /build/configure-webauthn-on -j "$jobs" "${focused[@]}"
ninja -C /build/configure-webauthn-on -j "$jobs" WebKit WPEWebProcess WPENetworkProcess WPEGPUProcess WPEInjectedBundle InspectorResources
# This path is owned by the checked dedicated build volume, never a live runtime.
python3 - <<'PYCLEAN'
from pathlib import Path
import shutil
path = Path('/build/auth-runtime-stage')
if path.is_symlink():
    raise SystemExit('Refusing redirected install stage')
if path.exists():
    shutil.rmtree(path)
PYCLEAN
DESTDIR=/build/auth-runtime-stage cmake --install /build/configure-webauthn-on
python3 /src/scripts/build-auth-engine.py --container-stage
