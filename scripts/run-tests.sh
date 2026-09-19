#!/usr/bin/env bash
set -euo pipefail
# Build + run host tests (no device or SDK): clang++ C++17, Qt 6, Python and Node 22+.
# First install the locked demo dependencies: npm ci --prefix tools/passkey-acceptance
cd "$(dirname "$0")/.."
mkdir -p build
fail=0
for t in tests/*_test.cpp; do
  # Qt suites have their own CMake targets below.
  case "$t" in tests/auth_*_test.cpp|tests/qtfb_client_test.cpp) continue;; esac
  name="$(basename "$t" .cpp)"
  if clang++ -std=c++17 -Wall -Wextra -o "build/$name" "$t"; then
    "./build/$name" || { echo "FAIL (runtime): $name"; fail=1; }
  else
    echo "FAIL (compile): $name"; fail=1
  fi
done
for suite in auth-policy auth-surface auth-passkey; do
  if cmake -S "tests/$suite" -B "build/$suite" -G Ninja \
      && cmake --build "build/$suite" \
      && ctest --test-dir "build/$suite" --output-on-failure; then
    :
  else
    echo "FAIL: $suite tests"; fail=1
  fi
done
# AppLoad's local socket and shared-memory transport uses the Linux ABI.
if [ "$(uname -s)" = Linux ]; then
  if cmake -S tests/qtfb -B build/qtfb -G Ninja \
      && cmake --build build/qtfb \
      && ctest --test-dir build/qtfb --output-on-failure; then
    :
  else
    echo "FAIL: AppLoad QTFB tests"; fail=1
  fi
fi
for t in tests/test_auth_engine_build.py tests/auth_build_recipe_test.py tests/auth_launcher_test.py \
         engine/auth-passkey-helper/tests/package_test.py \
         tests/auth-webauthn-provider/https_fixture_test.py \
         tools/passkey-acceptance/native/test-preparation.py; do
  if python3 "$t"; then :; else echo "FAIL (Python): $t"; fail=1; fi
done
if npm --prefix tools/passkey-acceptance run check \
    && npm --prefix tools/passkey-acceptance test; then
  :
else
  echo "FAIL: passkey acceptance tests (run npm ci --prefix tools/passkey-acceptance first)"; fail=1
fi
# Shell unit tests (launcher / installer no-brick logic) — pure bash + stubbed systemctl/mount.
for t in tests/*_test.sh; do
  [ -e "$t" ] || continue
  name="$(basename "$t" .sh)"
  if bash "$t"; then :; else echo "FAIL (shell): $name"; fail=1; fi
done
# Optional lint of the shipped shell (skip cleanly if shellcheck isn't installed).
if command -v shellcheck >/dev/null 2>&1; then
  for f in device/rmweb device/rmweb-env.sh device/install.sh device/auth/entry \
           tools/passkey-acceptance/native/launch; do
    [ -e "$f" ] && { shellcheck -s sh "$f" || { echo "FAIL (shellcheck): $f"; fail=1; }; }
  done
else
  echo "[tests] shellcheck not found — skipping shell lint"
fi
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
