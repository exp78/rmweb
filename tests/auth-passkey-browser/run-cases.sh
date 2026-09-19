#!/bin/sh
set -eu
if [ "$#" -ne 1 ] || [ ! -x "$1" ]; then
    echo "Usage: $0 /path/to/auth-passkey-browser-smoke" >&2
    exit 2
fi
for scenario in success abort navigation http-diagnostics; do
    "$1" "$scenario"
done
