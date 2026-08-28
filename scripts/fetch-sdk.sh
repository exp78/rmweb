#!/usr/bin/env bash
set -euo pipefail
# Download reMarkable's official "ferrari" aarch64 Yocto SDK installer into toolchain/sdk/.
# The installer is large (~467 MB) and gitignored; this script makes it reproducible.
# SHA-256 is pinned to the known-good build that produced our verified toolchain.
URL="https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/ferrari/remarkable-production-image-5.7.119-ferrari-public-aarch64-toolchain.sh"
SHA256="218847107b9fca7bff8fb92db01ef5f74ec3df283a9ff0fe3a6e62b64a8237b4"
SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)/toolchain/sdk"
out="$SDK_DIR/$(basename "$URL")"

verify() {
  if command -v sha256sum >/dev/null 2>&1; then echo "$SHA256  $out" | sha256sum -c -
  else echo "$SHA256  $out" | shasum -a 256 -c -; fi
}

mkdir -p "$SDK_DIR"
if [ -f "$out" ]; then
  echo "Already present, verifying checksum…"
  verify || { echo "CHECKSUM FAILED — removing $out"; rm -f "$out"; exit 1; }
  exit 0
fi
echo "Downloading ferrari aarch64 SDK (~467 MB)…"
curl -fL --progress-bar -o "$out" "$URL"
echo "Verifying SHA-256…"
verify || { echo "CHECKSUM FAILED — removing $out"; rm -f "$out"; exit 1; }
echo "Saved & verified: $out ($(du -h "$out" | cut -f1))"
