#!/bin/bash
# Runs INSIDE rmweb-sdk. Rebuild Mesa 24.0.9 with llvmpipe (multi-threaded + SIMD JIT software GL) instead
# of softpipe (single-threaded reference rasterizer = the ~6 s compositing bottleneck on rMPP).
# Uses the container's own arm64 LLVM 16 (Debian bookworm); its glibc 2.36 < device 2.39 -> runs on device.
# Output: build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so (now with llvmpipe) + build/llvm-bundle/libLLVM*.
set -e
set -o pipefail   # the meson/ninja pipes below must not mask a build failure

echo "### STAGE 1: install arm64 LLVM 16 in the container ###"
apt-get update -qq
apt-get install -y -qq llvm-16-dev python3-mako python3-packaging >/dev/null   # mako+packaging for Mesa meson
LC=$(command -v llvm-config-16 2>/dev/null || echo /usr/lib/llvm-16/bin/llvm-config)
ln -sf "$LC" /usr/local/bin/llvm-config
echo "llvm-config=$(llvm-config --version)  libdir=$(llvm-config --libdir)"

. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
# The native-file carries the full toolchain; drop OE cross vars so they don't fight meson (per build-wpe).
unset CC CXX CPP LD AR NM STRIP RANLIB OBJCOPY OBJDUMP READELF CFLAGS CXXFLAGS CPPFLAGS LDFLAGS ASFLAGS \
      OECORE_TUNE_CCARGS CONFIG_SITE
# Mesa's meson codegen needs python 'mako' AND 'packaging' (its version check falls back to distutils,
# which is GONE in py3.12). The SDK python3.12 has no pip, so drop the pure-python mako + markupsafe +
# packaging from the container's apt packages straight into its site-packages.
SP=$(python3 -c 'import site;print(site.getsitepackages()[0])')
cp -r /usr/lib/python3/dist-packages/mako /usr/lib/python3/dist-packages/markupsafe \
      /usr/lib/python3/dist-packages/packaging "$SP/" 2>/dev/null || true
python3 -c 'import mako,packaging;print("mako",mako.__version__,"packaging",packaging.__version__)' \
      || echo "WARN: mako/packaging still missing"
SR=$SDKTARGETSYSROOT
cp -a /work/build/stage/usr/.      "$SR/usr/" 2>/dev/null || true
cp -a /work/build/stage-mesa/usr/. "$SR/usr/" 2>/dev/null || true
export PATH="/usr/local/bin:$PATH"
export PKG_CONFIG_PATH="$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$SR"

echo "### STAGE 2: configure Mesa (fresh _b2) with llvm=enabled, shared-llvm ###"
cd /work/build/src/mesa-24.0.9
# Use meson.real (the raw meson) NOT the SDK 'meson' wrapper — the wrapper injects --cross-file which makes
# it a cross build and pulls a build-machine python without mako. meson.real + only our native-file = native.
MESON=$(command -v meson.real || command -v meson)
# mako lives in the container python's dist-packages; expose it to whatever python meson runs under.
export PYTHONPATH="/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}"
echo "meson=$MESON"
# Same options as the working softpipe build, but llvm enabled + shared (so we can bundle libLLVM).
MESA_OPTS="-Dgallium-drivers=swrast -Dvulkan-drivers= -Dplatforms= -Degl-native-platform=surfaceless \
 -Dglx=disabled -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dgbm=enabled \
 -Dllvm=enabled -Dshared-llvm=enabled -Dshared-glapi=enabled -Ddri3=disabled -Dgallium-vdpau=disabled \
 -Dgallium-va=disabled -Dgallium-xa=disabled -Dgbm-backends-path=/usr/lib/gbm -Dglvnd=false -Dosmesa=false \
 -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dgallium-opencl=disabled -Dgallium-rusticl=false \
 -Dprefix=/usr -Dbuildtype=release"
rm -rf _b2
"$MESON" setup _b2 --native-file=/work/engine/mesa-native.ini $MESA_OPTS 2>&1 | tail -n 25
echo "--- gallium drivers configured: ---"; grep -iE "gallium|llvm" _b2/meson-logs/meson-log.txt | grep -iE "llvmpipe|softpipe|llvm.*(yes|enabled|found)" | head || true

echo "### STAGE 3: build (ninja) ###"
ninja -C _b2 2>&1 | tail -n 15

echo "### STAGE 4: install + collect libLLVM + report NEEDED ###"
rm -rf /work/build/stage-mesa-llvm; DESTDIR=/work/build/stage-mesa-llvm ninja -C _b2 install >/dev/null
mkdir -p /work/build/llvm-bundle
cp -L "$(llvm-config --libdir)"/libLLVM-16.so* /work/build/llvm-bundle/ 2>/dev/null || \
  cp -L /usr/lib/aarch64-linux-gnu/libLLVM-16.so* /work/build/llvm-bundle/ 2>/dev/null || true
ls -l /work/build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so /work/build/llvm-bundle/ 2>&1
echo "=== swrast_dri.so NEEDED ==="; readelf -d /work/build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so | grep NEEDED
echo "=== libLLVM-16 NEEDED ==="; readelf -d /work/build/llvm-bundle/libLLVM-16.so.1 2>/dev/null | grep NEEDED || echo "(no libLLVM collected)"
echo "### DONE ###"
