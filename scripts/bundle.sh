#!/usr/bin/env bash
set -euo pipefail
# Assemble the WPE runtime bundle and deploy it to /home/root/rmweb on the Paper Pro.
# Ships our built .so (+ sqlite3/webp pulled from the SDK sysroot, which the device runtime
# lacks), the Mesa software-GL drivers, the WPE subprocess helpers, and WebKit resources +
# injected-bundle. The other ~24 deps (glib, icu, cairo, freetype, harfbuzz, openssl, libxml2,
# png/jpeg, ...) are reused from the device.
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env || true
HOST="${REMARKABLE_HOST:-10.11.99.1}"; DUSER="${REMARKABLE_USER:-${DEVICE_USER:-root}}"   # REMARKABLE_USER (.env) is canonical; DEVICE_USER = legacy fallback
S=build/stage/usr; M=build/stage-mesa/usr; B=build/bundle

rm -rf "$B"; mkdir -p "$B/lib/dri" "$B/lib/gio/modules" "$B/libexec" "$B/bin"
cp -a "$S"/lib/*.so*                "$B/lib/"
cp -a "$M"/lib/*.so*                "$B/lib/"
cp -a "$S"/lib/gio/modules/*.so     "$B/lib/gio/modules/"            2>/dev/null || true   # glib-networking (TLS)
cp -a "$M"/lib/dri/*.so             "$B/lib/dri/"                    2>/dev/null || true
# llvmpipe: multi-core + SIMD-JIT software rasterizer — ~64x faster page compositing than softpipe
# (fixes the ~6 s page turn; see the six-second-render memory). Replace the softpipe swrast driver with
# the llvm-enabled build, and bundle libLLVM + only the deps the device lacks (libz3/tinfo/edit/bsd/md).
# libxml2/ICU are NOT bundled (device provides libxml2). Run with GALLIUM_DRIVER=llvmpipe.
if [ -f build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so ]; then
  cp -a build/stage-mesa-llvm/usr/lib/dri/swrast_dri.so "$B/lib/dri/swrast_dri.so"
  # Copy each lib individually so a missing one WARNs loudly instead of aborting the bundle under `set -e`
  # (and so an incomplete set is obvious rather than a silent runtime dlopen failure on the device).
  for lib in libLLVM-16.so.1 libz3.so.4 libtinfo.so.6 libedit.so.2 libbsd.so.0 libmd.so.0; do
    if [ -f "build/llvm-bundle/$lib" ]; then cp -aL "build/llvm-bundle/$lib" "$B/lib/"
    else echo "[bundle] WARN: build/llvm-bundle/$lib MISSING — llvmpipe will fail to dlopen on device"; fi
  done
else
  echo "[bundle] WARN: no llvmpipe build (build/stage-mesa-llvm) — shipping softpipe (slow). Run engine/mesa-llvmpipe.incontainer.sh"
fi
cp -a "$S"/libexec/wpe-webkit-2.0   "$B/libexec/"
cp -a "$S"/lib/wpe-webkit-2.0       "$B/lib/"                        2>/dev/null || true   # injected-bundle
cp -a "$S"/share/wpe-webkit-2.0     "$B/share/"                      2>/dev/null || true   # resources
# Reader mode: vendored Mozilla Readability.js (+ isProbablyReaderable), injected on "Reader" (Apache-2.0).
mkdir -p "$B/share/reader"
cp -a engine/wpeqt/reader/Readability.js engine/wpeqt/reader/Readability-readerable.js "$B/share/reader/"
# The app binary itself — a release bundle must be self-contained. (Dev deploy via run-wpeqt-on-device.sh
# scp's a fresh build over this; here we ship whatever build/ currently holds so the tarball is complete.)
[ -f build/rmweb-wpeqt ] && cp -a build/rmweb-wpeqt "$B/bin/" \
  || echo "[bundle] WARN: build/rmweb-wpeqt missing — run scripts/build-wpeqt.sh (a release bundle needs the app binary)"
# Phase 5 — installable app: on-device launcher, shared env, installer, version stamp, and (layer B) the
# AppLoad/XOVI external-app manifest + icon + entry wrapper. These live under device/ in the repo and
# ship at the bundle root (the appload/ subdir ships as-is).
cp -a device/rmweb device/rmweb-env.sh device/install.sh device/appload-entry.sh "$B/"
chmod +x "$B/rmweb" "$B/install.sh" "$B/appload-entry.sh"
cp VERSION "$B/VERSION"   # single source of truth: the repo-root VERSION file
[ -d device/appload ] && cp -a device/appload "$B/"
[ -f device/icon.svg ] && cp -a device/icon.svg "$B/"

# rpath for the bundled runtime: the app's RUNPATH ($ORIGIN/../lib) covers only its DIRECT deps —
# transitive deps of the bundled .so (libsoup/libwebp/... pulled in by libWPEWebKit) resolve via each
# lib's OWN rpath. Give every bundled .so an $ORIGIN-relative rpath so the bundle also starts without
# LD_LIBRARY_PATH (rmweb-env.sh still sets it as a safety net). The libexec helpers end up under the
# /usr/libexec overlay on the device, so they get the ABSOLUTE bundle lib dir instead.
if docker image inspect rmweb-sdk >/dev/null 2>&1; then
  docker run --rm -v "$PWD:/work" -w /work rmweb-sdk bash -euc '
    rp() { [ -d "$1" ] || return 0; find "$1" -maxdepth 1 \( -name "*.so" -o -name "*.so.*" \) -type f -exec patchelf --set-rpath "$2" {} +; }
    rp build/bundle/lib                      "\$ORIGIN"
    rp build/bundle/lib/dri                  "\$ORIGIN/.."
    rp build/bundle/lib/gio/modules          "\$ORIGIN/../.."
    rp build/bundle/lib/wpe-webkit-2.0       "\$ORIGIN/.."
    rp build/bundle/bin                      "\$ORIGIN/../lib"
    find build/bundle/libexec/wpe-webkit-2.0 -maxdepth 1 -type f -exec patchelf --set-rpath /home/root/rmweb/lib {} +
  ' || rpath_fail="patchelf pass failed"
else
  rpath_fail="no rmweb-sdk docker image"
fi
if [ -n "${rpath_fail:-}" ]; then
  # Release 0.9.0 shipped with NO rpath at all because this only WARNed — make it fatal for tarballs.
  if [ -n "${RMWEB_PACKAGE_ONLY:-}" ]; then
    echo "[bundle] ERROR: $rpath_fail — a release bundle without rpath is broken (LD_LIBRARY_PATH does not cover transitive deps)" >&2
    exit 1
  fi
  echo "[bundle] WARN: $rpath_fail — bundled libs left without rpath (LD_LIBRARY_PATH required)"
fi

echo "[bundle] local size:"; du -sh "$B"
if [ -n "${RMWEB_PACKAGE_ONLY:-}" ]; then
  echo "[bundle] package-only: assembled $B (skipping device deploy)"
else
  # Atomic deploy: unpack into a staging dir on the device, then swap with mv (rename is atomic
  # on the same fs). A failed/interrupted transfer leaves the LIVE install untouched. If rmweb is
  # running (its .lock exists), abort BEFORE unpacking — never swap libs under a live instance.
  # The remote shell is BusyBox ash: no GNU-isms.
  echo "[bundle] deploying to $DUSER@$HOST:/home/root/rmweb (atomic staging) ..."
  tar -C "$B" -cf - . | ssh "$DUSER@$HOST" '
    set -e
    R=/home/root/rmweb; S=/home/root/.rmweb-staging.$$; O=/home/root/.rmweb-old.$$
    if [ -e "$R/.lock" ]; then
      echo "[deploy] ERROR: $R/.lock exists — rmweb is RUNNING on the device." >&2
      echo "[deploy] quit it first (or remove a proven-stale lock); deploy aborted BEFORE unpacking." >&2
      exit 1
    fi
    rm -rf "$S" "$O" /home/root/.rmweb-staging.* /home/root/.rmweb-old.*   # sweep leftovers of interrupted deploys
    mkdir -p "$S"
    if tar -C "$S" -xf -; then
      [ -d "$R" ] && mv "$R" "$O"   # current -> .old
      mv "$S" "$R"                  # staging -> live (atomic rename)
      rm -rf "$O"
      echo "[device] deployed. /home/root/rmweb:"
      du -sh "$R"; ls "$R"
    else
      rm -rf "$S"
      echo "[deploy] ERROR: untar failed — live install untouched" >&2
      exit 1
    fi
  '
fi
