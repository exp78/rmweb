#!/bin/sh
# Mesa software-GL recipe for the reMarkable Paper Pro — TRACKED in git (moved out of the
# gitignored build/src/ so a clean clone can reproduce the Mesa stage). This builds the BASE
# softpipe Mesa (EGL + GLES2 + GBM, swrast/softpipe, no LLVM); the llvmpipe variant (the one
# the bundle prefers) reuses the same source + engine/mesa-native.ini and lives in
# engine/mesa-llvmpipe.incontainer.sh.
#
# Build software Mesa (EGL + GLES2 + GBM, swrast/softpipe, no LLVM) for the
# reMarkable Paper Pro, as a NATIVE aarch64 build inside the rmweb-sdk container.
#
# Usage (inside container, repo mounted at /work):
#   . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
#   /work/engine/build-mesa.sh setup    # configure
#   /work/engine/build-mesa.sh build    # compile
#   /work/engine/build-mesa.sh install  # DESTDIR install into build/stage-mesa
set -e

MESA_DIR=/work/build/src/mesa-24.0.9
BUILD_DIR="$MESA_DIR/_b"
STAGE=/work/build/stage-mesa
NATIVE_FILE=/work/engine/mesa-native.ini

. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux 2>/dev/null
export SSL_CERT_DIR="$OECORE_NATIVE_SYSROOT/etc/ssl/certs/"
export PYTHONPATH=/work/build/pydeps${PYTHONPATH:+:$PYTHONPATH}
# Keep a copy of the SDK compiler/sysroot for the smoke target before we scrub
# the env for the (native-file-driven) meson build.
SDK_CC="$CC"
SDK_SYSROOT="$SDKTARGETSYSROOT"

# Use the real meson (bypass the OE wrapper which force-injects the cross-file).
MESON=meson.real
command -v "$MESON" >/dev/null 2>&1 || MESON=meson

# The OE wrapper exports CC/CXX as multi-word strings; meson's native file
# drives the build instead, so scrub these for setup/build/install.
unset CC CXX CPP LD AR NM STRIP CFLAGS CXXFLAGS LDFLAGS CPPFLAGS

case "$1" in
  setup)
    rm -rf "$BUILD_DIR"
    exec "$MESON" setup "$BUILD_DIR" "$MESA_DIR" \
      --native-file "$NATIVE_FILE" \
      --prefix /usr \
      --buildtype release \
      -Dgallium-drivers=swrast \
      -Dvulkan-drivers= \
      -Dplatforms= \
      -Degl-native-platform=surfaceless \
      -Dglx=disabled \
      -Degl=enabled \
      -Dgles1=disabled \
      -Dgles2=enabled \
      -Dopengl=true \
      -Dgbm=enabled \
      -Dllvm=disabled \
      -Dshared-glapi=enabled \
      -Ddri3=disabled \
      -Dgallium-vdpau=disabled \
      -Dgallium-va=disabled \
      -Dgallium-xa=disabled \
      -Dgbm-backends-path=/usr/lib/gbm \
      -Dglvnd=false \
      -Dosmesa=false \
      -Dvalgrind=disabled \
      -Dlibunwind=disabled \
      -Dzstd=disabled \
      -Dgallium-opencl=disabled \
      -Dgallium-rusticl=false
    ;;
  build)
    exec ninja -C "$BUILD_DIR" -j"$(nproc)"
    ;;
  install)
    rm -rf "$STAGE"
    DESTDIR="$STAGE" ninja -C "$BUILD_DIR" install
    ;;
  smoke)
    # Cross-compile + run the EGL surfaceless software-GL smoke test.
    # IMPORTANT: the runtime gallium pipe driver is named "softpipe"
    # (NOT "swrast" -- swrast is only the DRI *module* name). Setting
    # GALLIUM_DRIVER=swrast makes screen creation fail; use softpipe or
    # leave it unset (auto-selects softpipe when LLVM is absent).
    SMOKE=/work/build/src/egl-smoke
    SR="$SDK_SYSROOT"
    LOADER="$SR/usr/lib/ld-linux-aarch64.so.1"
    $SDK_CC "$SMOKE/egl_smoke.c" \
      -I"$STAGE/usr/include" \
      -L"$STAGE/usr/lib" -lEGL -lGLESv2 \
      -Wl,-rpath-link,"$STAGE/usr/lib" \
      -o "$SMOKE/egl_smoke"
    EGL_PLATFORM=surfaceless \
    LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=softpipe \
    LIBGL_DRIVERS_PATH="$STAGE/usr/lib/dri" \
    exec "$LOADER" --library-path "$STAGE/usr/lib:$SR/usr/lib:$SR/lib" "$SMOKE/egl_smoke"
    ;;
  *)
    echo "usage: $0 {setup|build|install|smoke}" >&2
    exit 2
    ;;
esac
