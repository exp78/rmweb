#!/bin/sh
set -eu
if [ "$#" -ne 1 ] || [ ! -x "$1" ]; then
    echo "Usage: $0 /path/to/native-webauthn-provider-test" >&2
    exit 2
fi
for scenario in success discoverable challenge-1025 challenge-16384 invalid-rp-hash invalid-up invalid-uv \
    invalid-at invalid-backup invalid-id invalid-large-auth invalid-large-signature \
    invalid-empty-user invalid-discoverable-user cancel abort navigation timeout \
    unhandled wrong-rp public-suffix http registration conditional silent \
    challenge-empty challenge-large capabilities unfocused child-blank child-srcdoc; do
    "$1" "$scenario"
done
