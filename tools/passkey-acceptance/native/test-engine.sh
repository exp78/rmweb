#!/bin/bash
# Four actual-engine route cases over trusted loopback TLS; no phone ceremony.
set -euo pipefail
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 ACCEPTANCE_BUILD_OUTPUT" >&2
    exit 2
fi
output=$(cd -- "$1" && pwd)
source_root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
python3 - "$source_root" "$output" <<'PY'
import importlib.util,json,sys
from pathlib import Path
root, output=map(Path,sys.argv[1:]); receipt=json.loads((output/'receipt.json').read_text())
spec=importlib.util.spec_from_file_location('auth_build',root/'scripts/build-auth-browser.py')
auth_build=importlib.util.module_from_spec(spec); spec.loader.exec_module(auth_build)
if any(auth_build.sha(root/p)!=h for p,h in receipt['source'].items()):
    raise SystemExit('Source changed since build; rebuild in fresh output')
if any(auth_build.sha(output/'build'/p)!=h for p,h in receipt['testArtifacts'].items()):
    raise SystemExit('Test artifact changed')
application=output/'application'
if auth_build.regular_files(application)!=set(receipt['artifacts']):
    raise SystemExit('Acceptance application inventory changed')
if any(auth_build.sha(application/p)!=h for p,h in receipt['artifacts'].items()):
    raise SystemExit('Acceptance application changed')
if (output/'sdk-image').read_text()!=receipt['sdkImage']:
    raise SystemExit('SDK image changed')
PY
sdk_image=$(cat "$output/sdk-image")
docker run --rm --network none --cap-add NET_ADMIN --platform linux/arm64 \
    --add-host login.example.com:127.0.0.1 \
    -v "$source_root:/src:ro" -v "$output/build:/acceptance:ro" \
    -v "$output/application:/opt/rmweb-passkey-acceptance:ro" \
    -v "$output/application/runtime/libexec:/usr/libexec:ro" "$sdk_image" timeout 90 bash -c '
set -euo pipefail
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
export LC_ALL=C.UTF-8
cp --remove-destination "$SDKTARGETSYSROOT/lib/ld-linux-aarch64.so.1" /lib/ld-linux-aarch64.so.1
R=/opt/rmweb-passkey-acceptance/runtime
export RMWEB_AUTH_RUNTIME="$R"
source "$R/rmweb-env.sh"
export LD_LIBRARY_PATH="$R/lib:$SDKTARGETSYSROOT/usr/lib"
unset QT_QPA_PLATFORM QT_QUICK_BACKEND QSG_RENDER_LOOP
python3 /src/tools/passkey-acceptance/native/run-engine.py /acceptance/acceptance-engine-test
'
