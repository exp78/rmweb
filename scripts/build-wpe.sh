#!/usr/bin/env bash
set -euo pipefail
# =============================================================================
# Cross-build WPE WebKit 2.48.5 (software / Skia CPU, no media) for the
# reMarkable Paper Pro (aarch64, NO GPU), then render a web page to a PNG to
# prove the engine works on software GL (Mesa softpipe, surfaceless EGL).
#
# Installs into build/stage/usr. Seeds the SDK sysroot from build/stage (our
# deps) AND build/stage-mesa (software EGL/GLES/GBM) first. Tarballs are fetched
# on the HOST (the container has no curl).
#
# Usage:
#   scripts/build-wpe.sh deps     # fetch+cross-build the extra deps WebKit needs
#   scripts/build-wpe.sh build    # configure + compile + install WPE WebKit
#   scripts/build-wpe.sh render   # build+run the PNG render proof -> build/wpe-render.png
#   scripts/build-wpe.sh all      # deps + build + render (default)
#
# ---- THE NATIVE-BUILD TRICK (why this works) --------------------------------
# The rmweb-sdk container IS aarch64 and the SDK cross-gcc emits binaries that
# run natively in it. WebKit runs generated host tools during the build
# (offlineasm in Ruby, bindings generators, glib codegen, ...). The OE CMake
# toolchain file (CMAKE_TOOLCHAIN_FILE=.../OEToolchainConfig.cmake) sets
# CMAKE_CROSSCOMPILING=TRUE and demands an exe-wrapper -> that breaks running
# those generated tools ("cannot run target binary"). So we UNSET that toolchain
# file + scrub the OE multi-word CC/CXX/CFLAGS env, and hand CMake the bare
# aarch64 gcc/g++ with the cpu+sysroot flags as a NORMAL (native) compiler:
# target==build, no cross, no exe-wrapper. (Mesa was built the same way via a
# meson native-file; libwpe below reuses that file.)
#
# ---- HOST BUILD TOOLS the debian-slim+SDK base lacks ------------------------
#   apt-get install: ruby gperf unifdef gettext flex bison perl
#                    libglib2.0-bin libglib2.0-dev-bin  (native glib-compile-resources)
#   PLUS prepend build/stage/usr/bin to PATH for glib-mkenums/glib-genmarshal
#   (python scripts staged earlier; the SDK does not ship them).
#
# ---- DEPS WebKit REQUIRES that were NOT pre-staged/in-SDK -------------------
# WPE 2.48.5 find_package() hard-requires these; we cross-build + stage them:
#   * libharfbuzz-icu : SDK harfbuzz 8.3.0 was built WITHOUT --with-icu, so the
#       hb_icu_* symbols (used by Source/.../ComplexTextControllerSkia.cpp) are
#       missing. We compile the single upstream src/hb-icu.cc (8.3.0) into
#       libharfbuzz-icu.so.0 + write harfbuzz-icu.pc. (find_package(HarfBuzz
#       REQUIRED COMPONENTS ICU))
#   * libtasn1        : OptionsWPE find_package(Libtasn1 REQUIRED) (WebCrypto EC).
#   * libwpe (wpe-1.0): OptionsWPE find_package(WPE REQUIRED) -- the low-level
#       libwpe interface lib, separate from WPEWebKit. Built via meson native-file.
#   * libxslt/libexslt: OptionsWPE find_package(LibXslt REQUIRED) (ENABLE_XSLT ON).
#
# ---- RENDER PATH (why no /dev/dri is needed) --------------------------------
# WPEDisplayHeadless uses EGL_PLATFORM_SURFACELESS_MESA. With NO DRM render node
# (no /dev/dri), UIProcess/glib/WebProcessPoolGLib only advertises the
# SharedMemory renderer-buffer transport (Hardware needs a render node). The GPU
# process's AcceleratedSurfaceDMABuf then takes the Surfaceless->SharedMemory
# path: it renders with GL and glReadPixels(GL_BGRA) into a ShareableBitmap that
# becomes a WPEBufferSHM. Our proof connects WPEView::buffer-rendered, calls
# wpe_buffer_import_to_pixels() (BGRA bytes, no GBM/DRM map needed), and writes
# a PNG. See build/wpe_render.c.
# =============================================================================

cd "$(dirname "$0")/.."
REPO="$PWD"
IMG=rmweb-sdk
VER=2.48.5
JOBS="${JOBS:-8}"
ACTION="${1:-all}"

# Versions of the extra deps we build:
HB_VER=8.3.0          # MUST match the SDK harfbuzz (pkg-config --modversion harfbuzz)
TASN1_VER=4.19.0
LIBWPE_VER=1.16.2
XSLT_VER=1.1.39

# ---------- host fetch helpers (container has no curl) ----------
fetch() { # url dest-dir [strip]
  local url="$1" dest="$2" strip="${3:-1}"
  [ -e "$dest" ] && [ -n "$(ls -A "$dest" 2>/dev/null)" ] && return 0
  echo "[fetch] $url"
  mkdir -p "$dest"
  local tmp; tmp="$(mktemp "${TMPDIR:-/tmp}/_dl.XXXXXX.tar")"
  curl -fL "$url" -o "$tmp"
  # On a corrupt/partial extract, wipe dest so the non-empty skip-guard above can't later
  # mistake a half-written tree for a good one.
  tar xf "$tmp" -C "$dest" --strip-components="$strip" || { rm -rf "$dest"; rm -f "$tmp"; return 1; }
  rm -f "$tmp"
}

fetch_sources() {
  fetch "https://wpewebkit.org/releases/wpewebkit-$VER.tar.xz"          "build/src/wpewebkit-$VER"
  fetch "https://github.com/harfbuzz/harfbuzz/releases/download/$HB_VER/harfbuzz-$HB_VER.tar.xz" "build/src/harfbuzz-$HB_VER"
  fetch "https://ftp.gnu.org/gnu/libtasn1/libtasn1-$TASN1_VER.tar.gz"   "build/src/libtasn1-$TASN1_VER"
  fetch "https://wpewebkit.org/releases/libwpe-$LIBWPE_VER.tar.xz"      "build/src/libwpe-$LIBWPE_VER"
  fetch "https://download.gnome.org/sources/libxslt/1.1/libxslt-$XSLT_VER.tar.xz" "build/src/libxslt-$XSLT_VER"
}

# ---------- run a script body inside the SDK container ----------
incon() { docker run --rm -v "$REPO":/work -v rmweb-build:/build -w /work -e JOBS="$JOBS" "$IMG" bash -lc "$1"; }

# =========================== STAGE: deps ===========================
build_deps() {
  fetch_sources
  incon '
    set -e
    . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
    STAGE=/work/build/stage
    cp -a "$STAGE/usr/." "$SDKTARGETSYSROOT/usr/" 2>/dev/null || true
    cp -a /work/build/stage-mesa/usr/. "$SDKTARGETSYSROOT/usr/" 2>/dev/null || true
    SR=$SDKTARGETSYSROOT
    export PKG_CONFIG_PATH="$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig"
    export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

    HBV='"$HB_VER"'; TAV='"$TASN1_VER"'; LWV='"$LIBWPE_VER"'; XSV='"$XSLT_VER"'
    HBSO=libharfbuzz-icu.so.0.$(echo $HBV | awk -F. "{printf \"%d%02d%d\",\$1,\$2,\$3}").0

    echo "########## libatomic (16-byte atomics; SDK toolchain ships none) ##########"
    # cortex-a53 is ARMv8.0 (no LSE) so __int128 atomics need out-of-line helpers,
    # but the SDK gcc has NO libatomic and -latomic cannot be found. WebKit/libpas
    # references __atomic_load_16/__atomic_store_16 -> jsc & libWPEPlatform fail to
    # link. We ship a tiny spinlock-based libatomic.so providing the 16-byte ops.
    if [ ! -e "$STAGE/usr/lib/libatomic.so" ]; then
      $CC -fPIC -O2 -c /work/engine/atomic16.c -o /tmp/atomic16.o
      $CC -shared -fPIC -Wl,-soname,libatomic.so.1 -o "$STAGE/usr/lib/libatomic.so.1.2.0" /tmp/atomic16.o
      ( cd "$STAGE/usr/lib"; ln -sf libatomic.so.1.2.0 libatomic.so.1; ln -sf libatomic.so.1 libatomic.so )
      cp -a "$STAGE/usr/lib/libatomic.so"* "$SR/usr/lib/"
    fi
    ls -l "$STAGE"/usr/lib/libatomic.so.1.*

    echo "########## libharfbuzz-icu ($HBV) -- single-file hb-icu.cc ##########"
    if [ ! -e "$STAGE/usr/lib/libharfbuzz-icu.so" ]; then
      mkdir -p /tmp/hbcfg
      # hb-config.hh does #ifdef HAVE_CONFIG_H -> provide a sparse config.h that just enables ICU.
      printf "#define HAVE_ICU 1\n#define HAVE_ICU_BUILTIN 0\n#define HB_NO_MT 1\n" > /tmp/hbcfg/config.h
      HB=/work/build/src/harfbuzz-$HBV
      $CXX -fPIC -O2 -std=c++17 -DHAVE_CONFIG_H -I/tmp/hbcfg -I"$HB/src" \
        $(pkg-config --cflags icu-uc) -c "$HB/src/hb-icu.cc" -o /tmp/hb-icu.o
      $CXX -shared -fPIC -Wl,-soname,libharfbuzz-icu.so.0 \
        -o "$STAGE/usr/lib/$HBSO" /tmp/hb-icu.o \
        -L"$SR/usr/lib" -lharfbuzz $(pkg-config --libs icu-uc) -Wl,-rpath-link,"$SR/usr/lib"
      ( cd "$STAGE/usr/lib"; ln -sf "$HBSO" libharfbuzz-icu.so.0; ln -sf libharfbuzz-icu.so.0 libharfbuzz-icu.so )
      cat > "$STAGE/usr/lib/pkgconfig/harfbuzz-icu.pc" <<PC
prefix=/usr
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: harfbuzz-icu
Description: HarfBuzz ICU integration
Version: $HBV
Requires: harfbuzz = $HBV icu-uc
Libs: -L\${libdir} -lharfbuzz-icu
Cflags: -I\${includedir}/harfbuzz
PC
      cp -a "$STAGE/usr/lib/libharfbuzz-icu."* "$SR/usr/lib/" ; cp "$STAGE/usr/lib/pkgconfig/harfbuzz-icu.pc" "$SR/usr/lib/pkgconfig/"
    fi
    ls -l "$STAGE"/usr/lib/libharfbuzz-icu.so.0.*

    echo "########## libtasn1 ($TAV) -- autotools cross ##########"
    if [ ! -e "$STAGE/usr/lib/libtasn1.so" ]; then
      cd /work/build/src/libtasn1-$TAV
      make distclean >/dev/null 2>&1 || true
      ./configure --host=aarch64-remarkable-linux --prefix=/usr \
        --disable-doc --disable-static --enable-shared CC="$CC" CFLAGS="-O2" >/tmp/tasn1.log 2>&1
      make -j"$JOBS" >>/tmp/tasn1.log 2>&1
      make install DESTDIR="$STAGE" >>/tmp/tasn1.log 2>&1
      cp -a "$STAGE/usr/lib/libtasn1."* "$SR/usr/lib/"; cp "$STAGE/usr/lib/pkgconfig/libtasn1.pc" "$SR/usr/lib/pkgconfig/"
    fi
    ls -l "$STAGE"/usr/lib/libtasn1.so.*

    echo "########## libwpe ($LWV) -- meson native-file ##########"
    if [ ! -e "$STAGE/usr/lib/libwpe-1.0.so" ]; then
      MESON=meson.real; command -v "$MESON" >/dev/null 2>&1 || MESON=meson
      ( unset CC CXX CPP LD AR NM STRIP CFLAGS CXXFLAGS LDFLAGS CPPFLAGS
        cd /work/build/src/libwpe-$LWV; rm -rf _b
        "$MESON" setup _b . --native-file /work/build/src/mesa-native.ini \
          --prefix /usr --buildtype release -Denable-xkb=true >/tmp/libwpe.log 2>&1
        ninja -C _b -j"$JOBS" >>/tmp/libwpe.log 2>&1
        DESTDIR="$STAGE" ninja -C _b install >>/tmp/libwpe.log 2>&1 )
      cp -a "$STAGE/usr/lib/libwpe-1.0.so"* "$SR/usr/lib/"; cp "$STAGE/usr/lib/pkgconfig/wpe-1.0.pc" "$SR/usr/lib/pkgconfig/"
      cp -a "$STAGE/usr/include/wpe-1.0" "$SR/usr/include/" 2>/dev/null || true
    fi
    ls -l "$STAGE"/usr/lib/libwpe-1.0.so.*

    echo "########## libxslt ($XSV) -- autotools cross (explicit libxml2 flags) ##########"
    if [ ! -e "$STAGE/usr/lib/libxslt.so" ]; then
      cd /work/build/src/libxslt-$XSV
      make distclean >/dev/null 2>&1 || true
      XML_CF=$(pkg-config --cflags libxml-2.0); XML_LI=$(pkg-config --libs libxml-2.0)
      ./configure --host=aarch64-remarkable-linux --prefix=/usr \
        --without-python --without-crypto --disable-static --enable-shared \
        CC="$CC" CFLAGS="-O2 $XML_CF" LDFLAGS="$XML_LI" \
        LIBXML_CFLAGS="$XML_CF" LIBXML_LIBS="$XML_LI" \
        ac_cv_path_XML_CONFIG="$SR/usr/bin/xml2-config" >/tmp/xslt.log 2>&1
      make -j"$JOBS" >>/tmp/xslt.log 2>&1
      make install DESTDIR="$STAGE" >>/tmp/xslt.log 2>&1
    fi
    ls -l "$STAGE"/usr/lib/libxslt.so.*
    echo "[deps] all extra deps staged."
  '
}

# =========================== STAGE: build WPE WebKit ===========================
build_wpe() {
  fetch_sources
  # The full, working build recipe lives in engine/build-wpe.incontainer.sh (self-contained:
  # host build-deps incl. g++, the native-build trick, sysroot-pinned pkg-config, a FRESH
  # configure on the case-sensitive /build volume, and a resumable ninja). incon mounts the
  # rmweb-build volume, so a killed build resumes from its objects instead of restarting.
  incon 'bash /work/engine/build-wpe.incontainer.sh'
}

# =========================== STAGE: render proof ===========================
render() { incon 'bash /work/engine/render-wpe.incontainer.sh'; }

case "$ACTION" in
  deps)   build_deps ;;
  build)  build_wpe ;;
  render) render ;;
  all)    build_deps; build_wpe; render ;;
  *) echo "usage: $0 {deps|build|render|all}"; exit 2 ;;
esac
