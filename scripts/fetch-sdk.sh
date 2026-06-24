#!/usr/bin/env bash
set -euo pipefail
# Download reMarkable's official "ferrari" aarch64 Yocto SDK installer into toolchain/sdk/.
# The installer is large (~467 MB) and gitignored; this script makes it reproducible.
URL="https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/ferrari/remarkable-production-image-5.7.119-ferrari-public-aarch64-toolchain.sh"
SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)/toolchain/sdk"
mkdir -p "$SDK_DIR"
out="$SDK_DIR/$(basename "$URL")"
if [ -f "$out" ]; then echo "Already present: $out"; exit 0; fi
echo "Downloading ferrari aarch64 SDK (~467 MB)…"
curl -fL --progress-bar -o "$out" "$URL"
echo "Saved: $out ($(du -h "$out" | cut -f1))"
