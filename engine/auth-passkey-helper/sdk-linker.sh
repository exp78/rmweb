#!/bin/bash
set -eu
output=
previous=
for argument in "$@"; do
    if [ "$previous" = -o ]; then output=$argument; break; fi
    previous=$argument
done
case "$output" in
    "$CARGO_TARGET_DIR"/aarch64-unknown-linux-gnu/*) ;;
    *) exec /usr/bin/cc "$@" ;;
esac
# Cargo's host loader path is incompatible with the SDK environment script.
unset LD_LIBRARY_PATH
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
# CC is the trusted SDK compiler plus its required sysroot/architecture flags.
exec $CC "$@"
