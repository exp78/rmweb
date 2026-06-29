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
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
