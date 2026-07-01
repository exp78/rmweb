#!/usr/bin/env bash
set -euo pipefail
# Produce a self-contained, installable release tarball for the reMarkable Paper Pro — NO toolchain needed to
# install it (the bundle already carries the app + every runtime lib with rpath set). Assembles the bundle via
# scripts/bundle.sh in package-only mode (no device deploy), then writes dist/rmweb-<VERSION>.tar.gz.
cd "$(dirname "$0")/.."

[ -f build/rmweb-wpeqt ] || { echo "[package] build/rmweb-wpeqt missing — run ./scripts/build-wpeqt.sh first"; exit 1; }
VER="$(grep '^VER=' device/install.sh | cut -d'"' -f2)"
[ -n "$VER" ] || { echo "[package] could not read VER from device/install.sh"; exit 1; }

echo "[package] assembling bundle (package-only, no deploy) ..."
RMWEB_PACKAGE_ONLY=1 ./scripts/bundle.sh

# Sanity: the assembled bundle must carry the app binary + the launcher (else the tarball is not installable).
for f in bin/rmweb-wpeqt rmweb rmweb-env.sh install.sh; do
  [ -e "build/bundle/$f" ] || { echo "[package] ERROR: build/bundle/$f missing — bundle is incomplete"; exit 1; }
done

mkdir -p dist
OUT="dist/rmweb-$VER.tar.gz"
echo "[package] writing $OUT ..."
tar -C build/bundle -czf "$OUT" .
echo "[package] done: $OUT  ($(du -h "$OUT" | cut -f1), $(find build/bundle -type f | wc -l | tr -d ' ') files)"

cat <<EOF

── Install on the reMarkable Paper Pro (no toolchain needed) ──────────────────
  1. Copy the archive to the device (USB):
       scp $OUT root@10.11.99.1:/home/root/
  2. Extract + wire up on the device:
       ssh root@10.11.99.1 'mkdir -p /home/root/rmweb \\
         && gunzip -c /home/root/rmweb-$VER.tar.gz | tar -C /home/root/rmweb -xf - \\
         && /home/root/rmweb/install.sh'
  3. Run it:
       ssh root@10.11.99.1 '/home/root/rmweb/rmweb https://example.com'
     (quit with the ⏻ button in the toolbar → returns to the reMarkable menu)
───────────────────────────────────────────────────────────────────────────────
EOF
