#!/bin/bash
# Run inside the ARM64 Rust1.90+ / official PaperPro3.28 SDK container.
set -euo pipefail
if [ "$#" -ne 2 ]; then
    echo 'usage: build-sdk.sh ABSOLUTE_STAGED_SOURCE ABSOLUTE_BUILD_CACHE' >&2
    exit 2
fi
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
test "$(qmake -query QT_VERSION)" = 6.10.3
case "$(/usr/local/cargo/bin/rustc --version)" in "rustc 1.90.0 "*) ;; *) exit 2 ;; esac
helper_source=$1
helper_cache=$2
case "$helper_source:$helper_cache" in /*:/*) ;; *) exit 2 ;; esac
mkdir -p "$helper_cache/target" "$helper_cache/cargo-home"
export CARGO_HOME="$helper_cache/cargo-home"
export CARGO_TARGET_DIR="$helper_cache/target"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$helper_source/sdk-linker.sh"
export CARGO_BUILD_TARGET=aarch64-unknown-linux-gnu
export PKG_CONFIG_ALLOW_CROSS=1
export RUSTFLAGS='-C link-arg=-Wl,-z,relro,-z,now'
helper_sysroot=/opt/remarkable-sdk/sysroots/cortexa53-crypto-remarkable-linux
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUNNER="$helper_sysroot/lib/ld-linux-aarch64.so.1 --library-path $helper_sysroot/lib:$helper_sysroot/usr/lib"
cd "$helper_source"
/usr/local/cargo/bin/cargo test --locked --tests
/usr/local/cargo/bin/cargo build --release --locked
python3 tests/process.py --binary "$CARGO_TARGET_DIR/aarch64-unknown-linux-gnu/release/rmweb-auth-passkey-helper" \
    --loader "$helper_sysroot/lib/ld-linux-aarch64.so.1" --library-path "$helper_sysroot/lib:$helper_sysroot/usr/lib"
