#!/bin/bash
# Runs INSIDE the rmweb-sdk container. Cross/native-builds WPE WebKit 2.48.5
# (software, Skia CPU, no media) into /work/build/stage/usr.
# Args: configonly | buildonly | (none = configure+build+install)
set -e

VER=2.48.5
# IMPORTANT: the /work bind mount comes from a macOS (APFS) CASE-INSENSITIVE volume.
# WebKit has headers that differ only by case across -I dirs (e.g.
# Platform/IPC/glib/ArgumentCodersGlib.h vs Shared/glib/ArgumentCodersGLib.h). On a
# case-insensitive FS, #include "ArgumentCodersGLib.h" wrongly resolves to the wrong
# file -> GeneratedSerializers.cpp fails ("incomplete type ArgumentCoder<GTlsCertificate>").
# So we COPY the source into the container's case-SENSITIVE overlay (/build/wpe) and
# build there; only the install goes back to the bind-mounted stage.
WPE_SRC_RO=/work/build/src/wpewebkit-$VER
WPE_SRC=/build/wpe/wpewebkit-$VER
STAGE=/work/build/stage
JOBS="${JOBS:-8}"

echo "[wpe] installing host build deps (ruby gperf unifdef gettext flex bison perl + native glib codegen)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# ruby/gperf/unifdef/gettext: WebKit host build tools the SDK/base image lack.
# libglib2.0-dev-bin: provides a NATIVE aarch64 glib-compile-resources (a host build
# tool the SDK does not ship). Version mismatch vs target glib is fine (codegen only).
apt-get install -y -qq ruby gperf unifdef gettext flex bison perl g++ \
  libglib2.0-bin libglib2.0-dev-bin >/dev/null 2>&1
echo "[wpe] host deps: ruby=$(ruby -e 'print RUBY_VERSION' 2>/dev/null) gperf=$(gperf --version|head -1) unifdef=$(command -v unifdef) glib-compile-resources=$(command -v glib-compile-resources)"

. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux

# Seed the SDK sysroot from BOTH stages so cmake/pkg-config find our deps + software EGL/GLES.
cp -a "$STAGE/usr/." "$SDKTARGETSYSROOT/usr/"
cp -a /work/build/stage-mesa/usr/. "$SDKTARGETSYSROOT/usr/"

# glib codegen tools (glib-mkenums/glib-genmarshal) live in build/stage/usr/bin; the SDK
# does NOT ship them and WebKit's glib bindings codegen needs them. Prepend to PATH.
export PATH="$STAGE/usr/bin:$PATH"
echo "[wpe] glib-mkenums -> $(command -v glib-mkenums)"

SR="$SDKTARGETSYSROOT"
CPUFLAGS="-mcpu=cortex-a53+crc+crypto -mbranch-protection=standard"

# --- KILL the OE cross machinery so CMake sees a NATIVE aarch64 build (target==build,
#     no cross, no exe-wrapper). This is what lets WebKit run its generated host tools. ---
unset CMAKE_TOOLCHAIN_FILE
unset CC CXX CPP LD AR NM STRIP RANLIB OBJCOPY OBJDUMP READELF
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS ASFLAGS
unset OECORE_TUNE_CCARGS CONFIG_SITE

export PKG_CONFIG_PATH="$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
# pkg-config -I/-L MUST be sysroot-absolute: an absolute -I path ignores --sysroot, so without
# this gio-unix-2.0 (and any sysroot-only header) resolves to the header-less container
# /usr/include and the build fails ("gio/gfiledescriptorbased.h: No such file or directory").
export PKG_CONFIG_SYSROOT_DIR="$SR"

# Copy source onto the case-sensitive overlay (skip if already there for resume).
if [ ! -f "$WPE_SRC/CMakeLists.txt" ]; then
  echo "[wpe] copying source to case-sensitive FS: $WPE_SRC"
  mkdir -p "$(dirname "$WPE_SRC")"
  cp -a "$WPE_SRC_RO" "$WPE_SRC"
  rm -rf "$WPE_SRC/_b"   # drop any stale host configure (baked /work case-INSENSITIVE paths) -> reconfigure fresh here
fi
cd "$WPE_SRC"

if [ "${1:-}" != "buildonly" ] && [ ! -f _b/build.ninja ]; then
  echo "[wpe] configuring (native aarch64, sysroot=$SR)"
  rm -rf _b
  cmake -S . -B _b -GNinja \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-remarkable-linux-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-remarkable-linux-g++ \
    -DCMAKE_C_FLAGS="$CPUFLAGS --sysroot=$SR" \
    -DCMAKE_CXX_FLAGS="$CPUFLAGS --sysroot=$SR" \
    -DCMAKE_EXE_LINKER_FLAGS="-latomic" \
    -DCMAKE_SHARED_LINKER_FLAGS="-latomic" \
    -DCMAKE_MODULE_LINKER_FLAGS="-latomic" \
    -DCMAKE_SYSROOT="$SR" \
    -DCMAKE_FIND_ROOT_PATH="$SR" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORT=WPE \
    -DUSE_SKIA=ON \
    -DUSE_GSTREAMER=OFF -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_SOURCE=OFF \
    -DENABLE_MEDIA_STREAM=OFF -DENABLE_WEB_CODECS=OFF -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DENABLE_WEBGL=OFF -DENABLE_WEBXR=OFF -DENABLE_WEB_RTC=OFF \
    -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=OFF -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    -DENABLE_INTROSPECTION=OFF -DENABLE_DOCUMENTATION=OFF \
    -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LIBHYPHEN=OFF -DENABLE_JOURNALD_LOG=OFF \
    -DENABLE_WPE_PLATFORM=ON \
    -DENABLE_WPE_PLATFORM_HEADLESS=ON \
    -DENABLE_WPE_PLATFORM_DRM=OFF \
    -DENABLE_WPE_PLATFORM_WAYLAND=OFF \
    -DENABLE_WPE_QT_API=OFF \
    -DUSE_ATK=OFF \
    -DUSE_WOFF2=OFF \
    -DUSE_LIBBACKTRACE=OFF \
    -DUSE_SYSPROF_CAPTURE=OFF \
    -DENABLE_MINIBROWSER=OFF \
    -DENABLE_COG=OFF \
    -DENABLE_API_TESTS=OFF \
    -DENABLE_WEBDRIVER=OFF
fi

if [ "${1:-}" = "configonly" ]; then
  echo "[wpe] CONFIGURE-ONLY done."
  exit 0
fi

echo "[wpe] building (-j$JOBS) ..."
ninja -C _b -j"$JOBS"
echo "[wpe] installing into $STAGE ..."
DESTDIR="$STAGE" ninja -C _b install
echo "[wpe] DONE:"
ls -l "$STAGE"/usr/lib/libWPEWebKit-*.so* 2>/dev/null | head
