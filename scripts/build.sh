#!/usr/bin/env bash
set -euo pipefail
# Run a cross-compile command inside the SDK container, with the repo mounted at /work
# and the ferrari SDK environment sourced.
# Usage: scripts/build.sh '<shell command, e.g. $CC -O2 hello/hello.c -o build/hello>'
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/work -w /work rmweb-sdk \
  bash -lc ". /opt/rmpp-sdk/environment-setup-* && $*"
