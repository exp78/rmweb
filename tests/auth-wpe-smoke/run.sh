#!/bin/bash
# Actual SDK/WPE input and navigation regressions. All runtime changes stay in disposable containers.
set -euo pipefail
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || { [ "$#" -eq 2 ] && [ "$2" != --public ]; }; then
    echo "Usage: $0 /absolute/path/to/completed-auth-build-cache [--public]" >&2
    exit 1
fi
source_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
auth_cache=$(cd -- "$1" && pwd)
test -f "$auth_cache/.rmweb-auth-build-cache"
test -f "$auth_cache/artifacts/manifest.json"
docker run --rm --network none --platform linux/arm64 \
    -v "$source_root:/src:ro" -v "$auth_cache:/work" \
    -v "$auth_cache/artifacts/runtime:/opt/rmweb-auth/runtime:ro" rmweb-app-sdk:3.28.0.172 bash -c '
set -euo pipefail
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
export LC_ALL=C.UTF-8
cp -a /work/stage/usr/. "$SDKTARGETSYSROOT/usr/"
cp -a /opt/rmweb-auth/runtime/lib/. "$SDKTARGETSYSROOT/usr/lib/"
export PKG_CONFIG_SYSROOT_DIR="$SDKTARGETSYSROOT"
export PKG_CONFIG_PATH="$SDKTARGETSYSROOT/usr/lib/pkgconfig:$SDKTARGETSYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
cmake -S /src/tests/auth-wpe-smoke -B /work/build-wpe-smoke -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-rpath-link,$SDKTARGETSYSROOT/usr/lib"
cmake --build /work/build-wpe-smoke -j4
cmake -S /src/tests/auth-webauthn-provider -B /work/build-wpe-provider -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-rpath-link,$SDKTARGETSYSROOT/usr/lib"
cmake --build /work/build-wpe-provider -j4
cmake -S /src/tests/auth-passkey-browser -B /work/build-passkey-browser -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-rpath-link,$SDKTARGETSYSROOT/usr/lib"
cmake --build /work/build-passkey-browser -j4
'
# The TLS fixture owns a dummy address for libc AI_ADDRCONFIG, with no route
# outside this disposable namespace. The public-page lane needs no capability.
docker run --rm --network none --cap-add NET_ADMIN \
    --add-host login.example.com:127.0.0.1 --platform linux/arm64 \
    -v "$auth_cache/artifacts/runtime:/opt/rmweb-auth/runtime:ro" \
    -v "$auth_cache/artifacts/runtime/libexec:/usr/libexec:ro" \
    -v "$auth_cache/build-wpe-smoke:/smoke:ro" \
    -v "$auth_cache/build-wpe-provider:/provider:ro" \
    -v "$auth_cache/build-passkey-browser:/passkey-browser:ro" \
    -v "$source_root/tests/auth-webauthn-provider:/provider-fixtures:ro" \
    -v "$source_root/tests/auth-passkey-browser/run-cases.sh:/run-passkey-browser-cases.sh:ro" \
    rmweb-app-sdk:3.28.0.172 timeout 180 bash -c '
set -euo pipefail
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
export LC_ALL=C.UTF-8
# Children need the same loader as the SDK libc selected below. This changes
# only the disposable container, never a mounted host or tablet system path.
cp --remove-destination "$SDKTARGETSYSROOT/lib/ld-linux-aarch64.so.1" /lib/ld-linux-aarch64.so.1
export RMWEB_AUTH_RUNTIME=/opt/rmweb-auth/runtime
source "$RMWEB_AUTH_RUNTIME/rmweb-env.sh"
export LD_LIBRARY_PATH="$RMWEB_AUTH_RUNTIME/lib:$SDKTARGETSYSROOT/usr/lib"
unset QT_QPA_PLATFORM QT_QUICK_BACKEND QSG_RENDER_LOOP
/smoke/auth-wpe-smoke
for scenario in child-blank child-srcdoc top-blank top-srcdoc data blob file popup cancel cancel-after-failure touch-link touch-focus pointer-link touch-drag touch-cancel pen-link; do
    /smoke/auth-navigation-smoke "$scenario"
done
/smoke/auth-frame-pacing-smoke static
/smoke/auth-frame-pacing-smoke animation
/smoke/auth-frame-pacing-smoke pump
/smoke/auth-frame-pacing-smoke continuous
bash /provider-fixtures/run-cases.sh /provider/native-webauthn-provider-test
bash /provider-fixtures/run-diagnostic-cases.sh /provider/native-webauthn-provider-test
python3 /provider-fixtures/run-https-cases.py /provider/native-webauthn-provider-test
bash /run-passkey-browser-cases.sh /passkey-browser/auth-passkey-browser-smoke
'

if [ "${2:-}" = --public ]; then
    # No credentials/code: fixed public device page, fresh ephemeral session.
    docker run --rm --platform linux/arm64 \
        -v "$auth_cache/artifacts/runtime:/opt/rmweb-auth/runtime:ro" \
        -v "$auth_cache/artifacts/runtime/libexec:/usr/libexec:ro" \
        -v "$auth_cache/build-wpe-smoke:/smoke:ro" rmweb-app-sdk:3.28.0.172 timeout 25 bash -c '
set -euo pipefail
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
export LC_ALL=C.UTF-8
cp --remove-destination "$SDKTARGETSYSROOT/lib/ld-linux-aarch64.so.1" /lib/ld-linux-aarch64.so.1
export RMWEB_AUTH_RUNTIME=/opt/rmweb-auth/runtime
source "$RMWEB_AUTH_RUNTIME/rmweb-env.sh"
export LD_LIBRARY_PATH="$RMWEB_AUTH_RUNTIME/lib:$SDKTARGETSYSROOT/usr/lib"
# SDK OpenSSL defaults to /usr/lib/ssl-3; use the container system trust bundle.
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
unset QT_QPA_PLATFORM QT_QUICK_BACKEND QSG_RENDER_LOOP
exec /smoke/auth-navigation-smoke public-device
'
fi
