#!/usr/bin/env bash
set -euo pipefail
# Produce a self-contained, installable release tarball for the reMarkable Paper Pro — NO toolchain needed to
# install it (the bundle already carries the app + every runtime lib with rpath set). Assembles the bundle via
# scripts/bundle.sh in package-only mode (no device deploy), then writes dist/rmweb-<VERSION>.tar.gz.
cd "$(dirname "$0")/.."

[ -f build/rmweb-wpeqt ] || { echo "[package] build/rmweb-wpeqt missing — run ./scripts/build-wpeqt.sh first"; exit 1; }
# Freshness: 0.9.0 already shipped a stale binary once. Refuse to package when any engine/wpeqt
# source is newer than the built app.
stale="$(find engine/wpeqt -type f -newer build/rmweb-wpeqt)"
if [ -n "$stale" ]; then
  echo "[package] ERROR: engine/wpeqt sources newer than build/rmweb-wpeqt — rebuild first: ./scripts/build-wpeqt.sh" >&2
  echo "$stale" | sed 's/^/[package]   /' >&2
  exit 1
fi
VER="$(cat VERSION)"   # single source of truth: the repo-root VERSION file
[ -n "$VER" ] || { echo "[package] could not read VERSION"; exit 1; }

echo "[package] assembling bundle (package-only, no deploy) ..."
RMWEB_PACKAGE_ONLY=1 ./scripts/bundle.sh

# Sanity: the assembled bundle must carry the app binary + the launcher (else the tarball is not installable).
for f in bin/rmweb-wpeqt rmweb rmweb-env.sh install.sh; do
  [ -e "build/bundle/$f" ] || { echo "[package] ERROR: build/bundle/$f missing — bundle is incomplete"; exit 1; }
done

# rpath gate: the tarball must be self-contained. Verify inside the SDK image (bundle.sh already
# required it in package-only mode): every shipped ELF in bin/, lib/, lib/dri/, lib/gio/modules/
# must have a NON-EMPTY rpath ($ORIGIN forms are fine); the libexec helpers run from the
# /usr/libexec overlay on the device, so they must carry exactly /home/root/rmweb/lib.
echo "[package] verifying bundle rpath ..."
docker run --rm -v "$PWD:/work" -w /work rmweb-sdk bash -euc '
  bad=0
  for f in build/bundle/bin/* build/bundle/lib/*.so* build/bundle/lib/dri/* build/bundle/lib/gio/modules/*; do
    [ -f "$f" ] || continue
    r="$(patchelf --print-rpath "$f" 2>/dev/null || true)"
    [ -n "$r" ] || { echo "[package] ERROR: no rpath: $f"; bad=1; }
  done
  for f in build/bundle/libexec/wpe-webkit-2.0/*; do
    [ -f "$f" ] || continue
    r="$(patchelf --print-rpath "$f" 2>/dev/null || true)"
    [ "$r" = /home/root/rmweb/lib ] || { echo "[package] ERROR: $f rpath is \"$r\", want /home/root/rmweb/lib"; bad=1; }
  done
  [ "$bad" = 0 ] || exit 1
' || { echo "[package] ERROR: bundle rpath check failed — see above"; exit 1; }

mkdir -p dist
OUT="dist/rmweb-$VER.tar.gz"
echo "[package] writing $OUT ..."
tar -C build/bundle -czf "$OUT" .
echo "[package] done: $OUT  ($(du -h "$OUT" | cut -f1), $(find build/bundle -type f | wc -l | tr -d ' ') files)"

cat <<EOF

── Install on the reMarkable Paper Pro (no toolchain needed) ──────────────────
  1. Copy the archive to the device (USB):
       scp $OUT root@10.11.99.1:/home/root/
  2. On the device: quit rmweb if it is running, then clean-install:
       ssh root@10.11.99.1 'rm -rf /home/root/rmweb && mkdir -p /home/root/rmweb \\
         && gunzip -c /home/root/rmweb-$VER.tar.gz | tar -C /home/root/rmweb -xf - \\
         && /home/root/rmweb/install.sh'
     (clean install wipes /home/root/rmweb — including rmweb.log; the profile in
      /home/root/.rmweb is NOT touched)
  3. Run it:
       ssh root@10.11.99.1 '/home/root/rmweb/rmweb https://example.com'
     (quit with the ⏻ button in the toolbar → returns to the reMarkable menu)
───────────────────────────────────────────────────────────────────────────────
EOF
