#!/usr/bin/env python3
"""Build only the authentication browser against verified isolated artifacts."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]
_engine_spec = importlib.util.spec_from_file_location('rmweb_auth_engine', REPOSITORY / 'scripts/build-auth-engine.py')
_engine_recipe = importlib.util.module_from_spec(_engine_spec)
_engine_spec.loader.exec_module(_engine_recipe)
RUNTIME_ENVIRONMENT = _engine_recipe.ENVIRONMENT
MARKER = '.rmweb-auth-build-cache'
MARKER_BYTES = b'rmweb-auth-build-cache-v1\n'
SDK_SHA = '65e5b98f9f7c83d857c5c720f7f6cb2d61fe20f4513dec023ad00aac27c85ae4'
RUNTIME_SHA = 'e7d6d169ec73743b9f6a60601eba607b741d2144c5b944bf828915b830aafb6b'
WPE_SHA = '01f36010705adb14404c56baf033147f7927cc7c6badec81bb141266fcdd8d0b'
HELPER_REVISION = 'a6bdb700918f4c8c06c52d41f4d2d9e79888c5ad'
TARGETS = ('rmweb-auth-browser', 'rmweb-auth-entry')
REQUIRED_RUNTIME = {'rmweb-env.sh', 'lib/libWPEWebKit-2.0.so.1',
    'libexec/wpe-webkit-2.0/WPEWebProcess', 'libexec/wpe-webkit-2.0/WPENetworkProcess',
    'libexec/wpe-webkit-2.0/WPEGPUProcess'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def external(path):
    path = Path(os.path.abspath(path.expanduser()))
    if path == REPOSITORY or REPOSITORY in path.parents:
        raise ValueError('authentication build inputs/cache must remain outside Git')
    for part in [path, *path.parents]:
        if part.is_symlink():
            raise ValueError('symlink build/input path')
    return path


def digest(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None


def relative(name):
    return (isinstance(name, str) and re.fullmatch(r'[A-Za-z0-9_+./-]{1,240}', name)
            and not name.startswith('/') and all(part not in ('', '.', '..') for part in name.split('/')))


def read_manifest(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError('missing or unsafe artifact manifest')
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError('artifact manifest must be an object')
    return value


def regular_files(directory):
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('expected a regular artifact directory')
    result = set()
    for path in directory.rglob('*'):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('artifact tree contains a redirected or special file')
        if path.is_file():
            result.add(path.relative_to(directory).as_posix())
    return result


def verify_files(directory, records):
    if not isinstance(records, dict) or not 1 <= len(records) <= 4096 or regular_files(directory) != set(records):
        raise ValueError('artifact file inventory mismatch')
    result = {}
    total = 0
    for name, record in records.items():
        if not relative(name) or not isinstance(record, dict):
            raise ValueError('unsafe artifact file record')
        mode = record.get('mode')
        if mode in ('0644', '0755'):
            mode = int(mode, 8)
        size = record.get('bytes')
        if (type(mode) is not int or mode not in (0o644, 0o755) or type(size) is not int
                or not 0 <= size <= 768 * 1024 * 1024 or not digest(record.get('sha256'))):
            raise ValueError('invalid artifact size/mode/hash')
        path = directory / name
        if path.stat().st_size != size or stat.S_IMODE(path.stat().st_mode) != mode or sha(path) != record['sha256']:
            raise ValueError('artifact differs from manifest')
        result[name] = {'sha256': record['sha256'], 'bytes': size, 'mode': mode}
        total += size
    if total > 2 * 1024 * 1024 * 1024:
        raise ValueError('artifact tree exceeds bound')
    return result


def executable(path):
    if path.is_symlink() or not path.is_file() or not path.stat().st_mode & 0o111 or path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError('invalid authentication executable')
    with path.open('rb') as stream:
        header = stream.read(64)
    if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
        raise ValueError('authentication executable is not AArch64 ELF')
    return {'sha256': sha(path), 'bytes': path.stat().st_size}


def helper_source_hash():
    source = REPOSITORY / 'engine/auth-passkey-helper'
    files = {str(path.relative_to(source)): sha(path) for path in sorted(source.rglob('*'))
        if path.is_file() and '__pycache__' not in path.parts
        and 'target' not in path.relative_to(source).parts and 'libwebauthn' not in path.relative_to(source).parts}
    return hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def validate_inputs(engine, helper):
    e = read_manifest(engine / 'manifest.json')
    if (e.get('schemaVersion'), e.get('purpose'), e.get('firmware')) != (1, 'auth-engine', '3.28.0.172'):
        raise ValueError('unsupported isolated engine profile')
    if (e.get('sdk', {}).get('sha256'), e.get('sdk', {}).get('qtVersion')) != (SDK_SHA, '6.10.3'):
        raise ValueError('engine SDK mismatch')
    if (e.get('upstream', {}).get('version'), e.get('upstream', {}).get('sourceSha256'), e.get('upstream', {}).get('runtimeSha256')) != ('2.48.5', WPE_SHA, RUNTIME_SHA):
        raise ValueError('engine upstream provenance mismatch')
    runtime = verify_files(engine / 'runtime', e.get('runtimeFiles'))
    devel = verify_files(engine / 'devel', e.get('develFiles'))
    if not REQUIRED_RUNTIME.issubset(runtime):
        raise ValueError('engine runtime incomplete')
    if (engine / 'runtime/rmweb-env.sh').read_text() != RUNTIME_ENVIRONMENT:
        raise ValueError('engine runtime launch contract differs from current checkout; rebuild engine artifacts')
    for name in runtime:
        if name not in ('rmweb-env.sh', 'VERSION') and name.split('/')[0] not in ('lib', 'libexec', 'share', 'licenses'):
            raise ValueError('unexpected runtime payload')
        if name.startswith('libexec/') and runtime[name]['mode'] != 0o755:
            raise ValueError('runtime process is not executable')
    if any(not (name.startswith('include/') or name.startswith('lib/pkgconfig/')) for name in devel):
        raise ValueError('unexpected development payload')
    patch = e.get('patch', {})
    if not relative(patch.get('path')) or not digest(patch.get('sha256')) or sha(engine / patch['path']) != patch['sha256']:
        raise ValueError('engine provider patch mismatch')
    if patch['sha256'] != sha(REPOSITORY / 'patches/wpe-2.48.5-native-assertion-provider.patch'):
        raise ValueError('engine provider patch differs from current checkout')
    if patch['path'] not in {'runtime/' + name for name in runtime}:
        raise ValueError('engine provider patch must ship in the runtime inventory')
    h = read_manifest(helper / 'manifest.json')
    if h.get('schemaVersion') != 1 or h.get('SDKsha256') != SDK_SHA or h.get('sourceRevision', {}).get('libwebauthn') != HELPER_REVISION:
        raise ValueError('phone helper provenance mismatch')
    if h.get('sourceRevision', {}).get('helperSourceSha256') != helper_source_hash():
        raise ValueError('phone helper source differs from current checkout')
    if h.get('patchSHA256') != sha(REPOSITORY / 'engine/auth-passkey-helper/patches/libwebauthn-buffered-response.patch'):
        raise ValueError('phone helper patch mismatch')
    binary = h.get('binary', {})
    if binary.get('path') != 'rmweb-auth-passkey' or executable(helper / 'rmweb-auth-passkey') != {key: binary.get(key) for key in ('sha256', 'bytes')}:
        raise ValueError('phone helper executable mismatch')
    records = h.get('licenses')
    if not isinstance(records, dict) or any(not name.startswith('licenses/') for name in records):
        raise ValueError('phone helper license inventory mismatch')
    licenses = verify_files(helper / 'licenses', {name.removeprefix('licenses/'): value for name, value in records.items()})
    if regular_files(engine) != {'manifest.json'} | {'runtime/' + n for n in runtime} | {'devel/' + n for n in devel}:
        raise ValueError('unexpected engine artifact files')
    if regular_files(helper) != {'manifest.json', 'rmweb-auth-passkey'} | {'licenses/' + n for n in licenses}:
        raise ValueError('unexpected helper artifact files')
    return e, h, runtime, devel, licenses



def validate_published(directory):
    manifest = read_manifest(directory / 'manifest.json')
    if manifest.get('schemaVersion') != 2 or manifest.get('purpose') != 'auth-browser':
        raise ValueError('refusing an unrecognized existing artifact tree')
    artifacts = manifest.get('artifacts', {})
    if set(artifacts) != {*TARGETS, 'rmweb-auth-passkey'}:
        raise ValueError('existing executable inventory mismatch')
    runtime = verify_files(directory / 'runtime', manifest.get('runtime', {}).get('files'))
    licenses = verify_files(directory / 'licenses', manifest.get('helper', {}).get('files'))
    for name, expected in artifacts.items():
        if executable(directory / name) != expected:
            raise ValueError('existing executable changed')
    if regular_files(directory) != {'manifest.json', *artifacts} | {'runtime/' + n for n in runtime} | {'licenses/' + n for n in licenses}:
        raise ValueError('existing artifacts contain unowned files')


def claim_cache(cache):
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    if cache.stat().st_uid != os.getuid() or stat.S_IMODE(cache.stat().st_mode) & 0o077:
        raise ValueError('build cache must be owned and private (0700)')
    if any(path.is_symlink() or not (path.is_file() or path.is_dir()) for path in cache.rglob('*')):
        raise ValueError('build cache contains a redirected or special file')
    marker = cache / MARKER
    if not marker.exists():
        if any(cache.iterdir()):
            raise ValueError('refusing a nonempty unmarked build cache')
        with marker.open('xb') as stream:stream.write(MARKER_BYTES)
    elif not marker.is_file() or marker.read_bytes() != MARKER_BYTES:
        raise ValueError('invalid build-cache ownership marker')
    # The supported smoke runner reuses this cache without changing artifacts.
    allowed = {MARKER, '.build.lock', 'stage', 'build-auth', 'artifacts', '.artifacts.previous',
               'build-wpe-smoke', 'build-wpe-provider', 'build-passkey-browser'}
    if any(path.name not in allowed and not path.name.startswith('.artifacts-stage-') for path in cache.iterdir()):
        raise ValueError('unexpected file in authentication cache')


def source_manifest():
    directory = REPOSITORY / 'engine/wpeqt'
    names = {'CMakeLists.txt', 'auth-entry.cpp', 'qtfbclient.cpp', 'qtfbclient.h'}
    names |= {path.name for path in directory.glob('auth-*') if path.suffix in ('.h', '.cpp')}
    result = {f'engine/wpeqt/{name}': sha(directory / name) for name in sorted(names)}
    for name in ('scripts/build-auth-browser.py', 'scripts/build-auth-engine.py', 'device/auth/entry'):
        result[name] = sha(REPOSITORY / name)
    return result


def build_cpp(cache, engine, image):
    command = r'''set -euo pipefail
source /opt/remarkable-sdk/environment-setup-cortexa53-crypto-remarkable-linux
test "$(qmake -query QT_VERSION)" = 6.10.3
cp -a /work/stage/usr/. "$SDKTARGETSYSROOT/usr/"
cp -a /engine/runtime/lib/. "$SDKTARGETSYSROOT/usr/lib/"
export PKG_CONFIG_SYSROOT_DIR="$SDKTARGETSYSROOT"
export PKG_CONFIG_PATH="$SDKTARGETSYSROOT/usr/lib/pkgconfig:$SDKTARGETSYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
cmake -S /src/engine/wpeqt -B /work/build-auth -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-rpath-link,$SDKTARGETSYSROOT/usr/lib"
cmake --build /work/build-auth --target rmweb-auth-browser rmweb-auth-entry -j4
for artifact in rmweb-auth-browser rmweb-auth-entry; do
    aarch64-remarkable-linux-readelf -h "/work/build-auth/$artifact"
    aarch64-remarkable-linux-readelf -d "/work/build-auth/$artifact"
done
'''
    subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--platform', 'linux/arm64',
        '--volume', f'{REPOSITORY}:/src:ro', '--volume', f'{cache}:/work',
        '--volume', f'{engine}:/engine:ro', image['Id'], 'bash', '-c', command], check=True)


def build(cache, engine, helper, sdk_image):
    e, h, runtime, devel, licenses = validate_inputs(engine, helper)
    claim_cache(cache)
    descriptor = os.open(cache / '.build.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'a') as lock:
        try:fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:raise ValueError('another authentication build owns this cache')
        for previous_name in ('artifacts', '.artifacts.previous'):
            if (cache / previous_name).exists():validate_published(cache / previous_name)
        image = json.loads(subprocess.check_output(['docker', 'image', 'inspect', sdk_image]))[0]
        if image.get('Architecture') != 'arm64' or image.get('Os') != 'linux' or not re.fullmatch(r'sha256:[0-9a-f]{64}', image.get('Id', '')):
            raise ValueError('SDK image must target Linux ARM64')
        before = source_manifest()
        inputs_before = (sha(engine / 'manifest.json'), sha(helper / 'manifest.json'))
        for name in ('stage', 'build-auth'):
            if (cache / name).exists():shutil.rmtree(cache / name)
        (cache / 'stage').mkdir()
        shutil.copytree(engine / 'devel', cache / 'stage/usr')
        build_cpp(cache, engine, image)
        if before != source_manifest():
            raise ValueError('authentication sources changed during the build')
        validate_inputs(engine, helper)
        if inputs_before != (sha(engine / 'manifest.json'), sha(helper / 'manifest.json')):
            raise ValueError('authentication inputs changed during the build')
        pending = Path(tempfile.mkdtemp(prefix='.artifacts-stage-', dir=cache))
        try:
            artifacts = {}
            for name in TARGETS:
                artifacts[name] = executable(cache / 'build-auth' / name)
                shutil.copy2(cache / 'build-auth' / name, pending / name)
            shutil.copy2(helper / 'rmweb-auth-passkey', pending / 'rmweb-auth-passkey')
            artifacts['rmweb-auth-passkey'] = executable(pending / 'rmweb-auth-passkey')
            shutil.copytree(engine / 'runtime', pending / 'runtime')
            shutil.copytree(helper / 'licenses', pending / 'licenses')
            verify_files(pending / 'runtime', runtime)
            verify_files(pending / 'licenses', licenses)
            manifest = {'schemaVersion':2, 'purpose':'auth-browser',
                'sdk':{'imageId':image['Id'], 'firmware':'3.28.0.172', 'qtVersion':'6.10.3', 'requiredInstallerSHA256':SDK_SHA},
                'runtime':{'baseArchiveSHA256':RUNTIME_SHA, 'wpeVersion':'2.48.5', 'wpeSourceSHA256':WPE_SHA,
                    'providerPatchSHA256':e['patch']['sha256'], 'files':runtime},
                'helper':{'sourceRevision':HELPER_REVISION, 'patchSHA256':h['patchSHA256'], 'files':licenses},
                'sources':before, 'engineManifestSHA256':inputs_before[0], 'helperManifestSHA256':inputs_before[1],
                'develFiles':devel, 'artifacts':artifacts}
            (pending / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            previous = cache / '.artifacts.previous'
            destination = cache / 'artifacts'
            if destination.exists():
                if previous.exists():shutil.rmtree(previous)
                destination.rename(previous)
            try:pending.rename(destination)
            except Exception:
                if previous.exists() and not destination.exists():previous.rename(destination)
                raise
        finally:
            if pending.exists():shutil.rmtree(pending)
    return cache / 'artifacts'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--engine-artifacts', type=Path, required=True)
    parser.add_argument('--helper-artifacts', type=Path, required=True)
    parser.add_argument('--sdk-image', default='rmweb-app-sdk:3.28.0.172')
    args = parser.parse_args()
    cache, engine, helper = map(external, (args.cache, args.engine_artifacts, args.helper_artifacts))
    if any(a == b or a in b.parents or b in a.parents for a, b in [(cache,engine),(cache,helper),(engine,helper)]):
        raise ValueError('cache and artifact inputs must be separate trees')
    print(build(cache, engine, helper, args.sdk_image))


if __name__ == '__main__':
    try:main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Authentication build failed: {error}')
