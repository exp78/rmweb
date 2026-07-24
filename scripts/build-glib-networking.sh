#!/usr/bin/env bash
set -euo pipefail
# Cross-build glib-networking (OpenSSL backend) -> the GIO TLS module libgioopenssl.so, so that
# https:// works in the WPE/libsoup stack. Installs into build/stage/usr/lib/gio/modules.
#
# THE MODULE (libgioopenssl.so) is a TARGET aarch64 ELF, compiled by the SDK cross-gcc against the
# target sysroot (gio-2.0 2.78.6 + openssl 3.2.6) via the SDK *cross* pkg-config. Its NEEDED libs
# (libgio/gobject/glib-2.0, libssl/libcrypto.so.3, libc) all ship on the device.
#
# WHY THE EARLIER ATTEMPT FAILED (and the fix):
#   The SDK `meson` is a WRAPPER that auto-injects BOTH a --cross-file (target: cross-gcc + cross
#   pkg-config) AND a --native-file (build machine: `gcc`, `pkg-config-native`). meson's
#   `import('gnome')` + `gnome.post_install(gio_querymodules=...)` need BUILD-MACHINE (native)
#   tooling that the debian-slim base lacks: a binary literally named `pkg-config-native`, a native
#   gio-2.0.pc, and a native `gio-querymodules`. Without them setup dies with
#   "Found pkg-config: NO", "Build-time dependency gio-2.0 found: NO",
#   "Program 'gio-querymodules' not found".
#   FIX: apt-get the native debian glib tooling + pkg-config, then symlink pkg-config-native ->
#   pkg-config. The rmweb-sdk container is itself aarch64, so debian's gio-querymodules /
#   glib-compile-resources are runnable native tools (codegen only; their version need not match
#   the target). The TARGET gio-2.0 for the module still resolves via the SDK cross pkg-config, so
#   the produced .so is an aarch64 ELF (verified with aarch64-remarkable-linux-readelf -h).
#   NOTE: setup still prints "Build-time dependency gio-2.0 found: NO" -- that is the NATIVE gio the
#   gnome module probes; it is non-fatal because gnome.post_install only needs the gio-querymodules
#   PROGRAM (found). The runtime does NOT need giomodule.cache either (we load via GIO_EXTRA_MODULES).
#
# New container deps (native, codegen-only): pkg-config libglib2.0-bin libglib2.0-dev libglib2.0-dev-bin.
# (See research-reuse.md §8.)
cd "$(dirname "$0")/.."
VER=2.78.1
# Pinned SHA-256 of the release tarball (same discipline as scripts/fetch-sdk.sh).
SHA256=e48f2ddbb049832cbb09230529c5e45daca9f0df0eda325f832f7379859bf09f
SRC="build/src/glib-networking-$VER"

verify_sha256() { # file expected-sha256
  if command -v sha256sum >/dev/null 2>&1; then echo "$2  $1" | sha256sum -c -
  else echo "$2  $1" | shasum -a 256 -c -; fi
}

if [ ! -f "$SRC/meson.build" ]; then
  echo "[fetch] glib-networking $VER (host)"
  mkdir -p "$SRC"
  tmp="$(mktemp "${TMPDIR:-/tmp}/_gn.XXXXXX")"   # template must END in the X's (GNU mktemp); tar auto-detects the format
  curl -fL "https://download.gnome.org/sources/glib-networking/2.78/glib-networking-$VER.tar.xz" -o "$tmp"
  # Verify BEFORE extracting; drop the tarball and the (empty) dest on mismatch.
  verify_sha256 "$tmp" "$SHA256" || { echo "[fetch] CHECKSUM FAILED — removing $tmp" >&2; rm -f "$tmp"; rm -rf "$SRC"; exit 1; }
  tar xf "$tmp" -C "$SRC" --strip-components=1 || { rm -rf "$SRC"; rm -f "$tmp"; exit 1; }
  rm -f "$tmp"
fi
docker run --rm -v "$PWD":/work -w /work rmweb-sdk bash -lc '
  set -e
  # --- native (build-machine) glib codegen + pkg-config the gnome meson module needs ---
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  # libglib2.0-dev-bin: native gio-querymodules / glib-compile-resources. libglib2.0-dev: native
  # gio-2.0.pc. pkg-config: the native pkg-config binary. (aarch64 debian -> all runnable here.)
  apt-get install -y -qq pkg-config libglib2.0-bin libglib2.0-dev libglib2.0-dev-bin >/dev/null 2>&1
  # meson native-file names the build-machine pkg-config "pkg-config-native"; provide it.
  ln -sf "$(command -v pkg-config)" /usr/local/bin/pkg-config-native

  . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
  SR=$SDKTARGETSYSROOT; STAGE=/work/build/stage
  mkdir -p "$STAGE/usr"
  cp -a "$STAGE/usr/." "$SR/usr/" 2>/dev/null || true
  export PATH="$STAGE/usr/bin:$PATH"   # any staged glib helpers (harmless if absent)

  cd /work/build/src/glib-networking-'"$VER"'
  rm -rf _b
  # Use the SDK meson WRAPPER (auto-injects the target cross-file + the native-file). nofallback:
  # never download/build subproject wraps (the tree ships a glib.wrap) -> use the sysroot glib/gio
  # (cross pkg-config finds 2.78.6); error clearly if a system dep is truly missing.
  meson setup _b --prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
    -Dopenssl=enabled -Dgnutls=disabled -Dlibproxy=disabled \
    -Dgnome_proxy=disabled -Denvironment_proxy=enabled -Dinstalled_tests=false
  ninja -C _b
  DESTDIR="$STAGE" ninja -C _b install
  echo "[gn] installed GIO TLS module(s):"; ls -l "$STAGE"/usr/lib/gio/modules/
  echo "[gn] arch check (must say AArch64):"
  aarch64-remarkable-linux-readelf -h "$STAGE/usr/lib/gio/modules/libgioopenssl.so" | grep -E "Class|Machine"
  echo "[gn] NEEDED libs (all on device: glib2.78, openssl3, libc):"
  aarch64-remarkable-linux-readelf -d "$STAGE/usr/lib/gio/modules/libgioopenssl.so" | grep NEEDED
'
