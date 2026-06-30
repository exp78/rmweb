#!/usr/bin/env bash
set -euo pipefail
# Build + run ALL pure-logic host unit tests (no device, no SDK). clang++ C++17.
cd "$(dirname "$0")/.."
mkdir -p build
fail=0
for t in tests/*_test.cpp; do
  name="$(basename "$t" .cpp)"
  if clang++ -std=c++17 -Wall -Wextra -o "build/$name" "$t"; then
    "./build/$name" || { echo "FAIL (runtime): $name"; fail=1; }
  else
    echo "FAIL (compile): $name"; fail=1
  fi
done
# Shell unit tests (launcher / installer no-brick logic) — pure bash + stubbed systemctl/mount.
for t in tests/*_test.sh; do
  [ -e "$t" ] || continue
  name="$(basename "$t" .sh)"
  if bash "$t"; then :; else echo "FAIL (shell): $name"; fail=1; fi
done
# Optional lint of the shipped shell (skip cleanly if shellcheck isn't installed).
if command -v shellcheck >/dev/null 2>&1; then
  for f in device/rmweb device/rmweb-env.sh device/install.sh; do
    [ -e "$f" ] && { shellcheck -s sh "$f" || { echo "FAIL (shellcheck): $f"; fail=1; }; }
  done
else
  echo "[tests] shellcheck not found — skipping shell lint"
fi
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
