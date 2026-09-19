#!/bin/sh
set -eu
if [ "$#" -ne 1 ] || [ ! -x "$1" ]; then
    echo "Usage: $0 /path/to/native-webauthn-provider-test" >&2
    exit 2
fi
for scenario in success challenge-1025 challenge-16384 cancel abort navigation timeout unhandled wrong-rp public-suffix \
    conditional silent challenge-empty challenge-large child-blank child-srcdoc \
    option-appid option-credProps option-largeBlob option-prf allow-large id-empty id-large; do
    "$1" "diagnostic-$scenario"
done
