#!/bin/bash
# Local-only ARM64 test launcher build; does not deploy or change production caches.
set -euo pipefail
if [ "$#" -ne 4 ]; then
    echo "Usage: $0 HTTPS_TEST_ORIGIN ENGINE_ARTIFACTS HELPER_ARTIFACTS FRESH_OUTPUT_DIRECTORY" >&2
    exit 2
fi
origin=$1
engine=$(cd -- "$2" && pwd)
helper=$(cd -- "$3" && pwd)
output=$4
source_root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
# Require a fresh output directory: never replace or adopt an existing cache.
mkdir -m 700 -- "$output"
output=$(cd -- "$output" && pwd)
python3 - "$engine" "$helper" "$source_root" "$output" "$origin" <<'PY'
import importlib.util, json, re, sys
from pathlib import Path
engine, helper, root, output = map(Path, sys.argv[1:5]); origin = sys.argv[5]
sys.path.insert(0, str(root/'tools/passkey-acceptance/native'))
from catalog import manifest as catalog_manifest
catalog_manifest(origin)
spec = importlib.util.spec_from_file_location('auth_build', root/'scripts/build-auth-browser.py')
auth_build = importlib.util.module_from_spec(spec); spec.loader.exec_module(auth_build)
manifest, helper_manifest, _, _, _ = auth_build.validate_inputs(engine, helper)
if not re.fullmatch(r'sha256:[0-9a-f]{64}', manifest['sdk']['imageId']): raise SystemExit('Invalid SDK image')
(output / 'sdk-image').write_text(manifest['sdk']['imageId'])
paths = list((root / 'tools/passkey-acceptance/native').glob('*'))
paths += list((root / 'engine/wpeqt').glob('auth-*'))
paths += [root / 'engine/wpeqt' / n for n in ['qtfbclient.h', 'qtfbclient.cpp', 'auth-entry.cpp']]
paths += [root/p for p in ['tests/auth-webauthn-provider/run-https-cases.py', 'tests/auth-webauthn-provider/https-server.py', 'tests/auth_entry_test.py', 'device/appload/rmweb/icon.png']]
paths += [root/p for p in ['scripts/build-auth-browser.py', 'scripts/build-auth-engine.py']]
inputs = {str(p.relative_to(root)): auth_build.sha(p) for p in paths if p.is_file()}
(output / 'inputs.json').write_text(json.dumps({'source': inputs, 'origin': origin,
    'engineManifestSHA256': auth_build.sha(engine / 'manifest.json'),
    'helperManifestSHA256': auth_build.sha(helper / 'manifest.json'),
    'sdkImage': manifest['sdk']['imageId']}, indent=2)+'\n')
PY
sdk_image=$(cat "$output/sdk-image")
docker run --rm --network none --platform linux/arm64 \
    -v "$source_root:/src:ro" -v "$engine:/engine:ro" -v "$output:/work" \
    -e "TEST_ORIGIN=$origin" "$sdk_image" bash -c '
set -euo pipefail
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
export LC_ALL=C.UTF-8
test "$(qmake -query QT_VERSION)" = 6.10.3
cp -a /engine/devel/. "$SDKTARGETSYSROOT/usr/"
cp -a /engine/runtime/lib/. "$SDKTARGETSYSROOT/usr/lib/"
export PKG_CONFIG_SYSROOT_DIR="$SDKTARGETSYSROOT"
export PKG_CONFIG_PATH="$SDKTARGETSYSROOT/usr/lib/pkgconfig:$SDKTARGETSYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
cmake -S /src/tools/passkey-acceptance/native -B /work/build -G Ninja -DTEST_ORIGIN="$TEST_ORIGIN" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-rpath-link,$SDKTARGETSYSROOT/usr/lib"
cmake --build /work/build -j4
"$SDKTARGETSYSROOT/lib/ld-linux-aarch64.so.1" --library-path "$SDKTARGETSYSROOT/usr/lib" /work/build/acceptance-policy-test
'
python3 - "$source_root" "$output" "$engine" "$helper" <<'PY'
import importlib.util, json, shutil, sys
from pathlib import Path
root, output, engine, helper = map(Path, sys.argv[1:]); receipt = json.loads((output/'inputs.json').read_text())
spec = importlib.util.spec_from_file_location('auth_build', root/'scripts/build-auth-browser.py')
auth_build = importlib.util.module_from_spec(spec); spec.loader.exec_module(auth_build)
if any(auth_build.sha(root/p) != h for p,h in receipt['source'].items()):
    raise SystemExit('Source changed during build')
_, helper_manifest, runtime, _, licenses = auth_build.validate_inputs(engine, helper)
if (auth_build.sha(engine/'manifest.json') != receipt['engineManifestSHA256'] or
        auth_build.sha(helper/'manifest.json') != receipt['helperManifestSHA256']):
    raise SystemExit('Artifact inputs changed during build')
application = output/'application'; application.mkdir(mode=0o700)
(application/'bin').mkdir(mode=0o700)
for source, destination in [('acceptance-browser','rmweb-auth-browser'),('acceptance-entry','rmweb-auth-entry')]:
    expected = auth_build.executable(output/'build'/source)
    path = application/'bin'/destination
    shutil.copy2(output/'build'/source, path); path.chmod(0o700)
    if auth_build.executable(path) != expected: raise SystemExit('Test executable changed during packaging')
shutil.copy2(helper/'rmweb-auth-passkey', application/'bin/rmweb-auth-passkey')
(application/'bin/rmweb-auth-passkey').chmod(0o700)
if auth_build.executable(application/'bin/rmweb-auth-passkey') != {k: helper_manifest['binary'][k] for k in ('sha256', 'bytes')}:
    raise SystemExit('Phone helper changed during packaging')
shutil.copytree(engine/'runtime', application/'runtime')
shutil.copytree(helper/'licenses', application/'licenses')
auth_build.verify_files(application/'runtime', runtime)
auth_build.verify_files(application/'licenses', licenses)
shutil.copyfile(root/'tools/passkey-acceptance/native/launch', application/'launch'); (application/'launch').chmod(0o700)
receipt['artifacts'] = {name: auth_build.sha(application/name) for name in sorted(auth_build.regular_files(application))}
receipt['testArtifacts'] = {n: auth_build.sha(output/'build'/n) for n in ['acceptance-policy-test','acceptance-engine-test']}
(output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
PY
python3 "$source_root/tools/passkey-acceptance/native/catalog.py" --origin "$origin" --output "$output/catalog"
echo "Local acceptance launcher build complete; physical proof pending"
