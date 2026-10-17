#!/usr/bin/env python3
"""Build the isolated WPE passkey engine; never modify the shared rmweb runtime."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

REPOSITORY = Path(__file__).resolve().parents[1]
LOCK = REPOSITORY / 'toolchain/auth-engine-inputs.json'
PATCH = REPOSITORY / 'patches/wpe-2.48.5-native-assertion-provider.patch'
SOURCE_FILES = REPOSITORY / 'patches/wpe-2.48.5-native-assertion-provider-files.json'
MARKER = b'rmweb-auth-engine-cache-v1\n'
VOLUME_MARKER = 'rmweb-auth-webauthn-engine-v1\n'
ENGINE = 'wpewebkit-2.48.5'
RUNTIME_VERSION = 'rmweb-auth WPEWebKit 2.48.5 native assertion provider\n'
ENVIRONMENT = '''# Isolated rmweb authentication runtime; no profile or diagnostic overrides.
case "${RMWEB_AUTH_RUNTIME:-}" in
    /*) ;;
    *) echo "RMWEB_AUTH_RUNTIME must name an absolute runtime directory" >&2; return 1 ;;
esac
export LD_LIBRARY_PATH="$RMWEB_AUTH_RUNTIME/lib"
export LIBGL_DRIVERS_PATH="$RMWEB_AUTH_RUNTIME/lib/dri"
export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_INJECTED_BUNDLE_PATH="$RMWEB_AUTH_RUNTIME/lib/wpe-webkit-2.0/injected-bundle"
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1 WEBKIT_SKIA_CPU_PAINTING_THREADS=0
export WEBKIT_DISABLE_ASYNC_SCROLLING=1 WEBKIT_FORCE_VBLANK_TIMER=1
export GIO_EXTRA_MODULES="$RMWEB_AUTH_RUNTIME/lib/gio/modules"
export FONTCONFIG_PATH=/etc/fonts HOME=/home/root
export JSC_useJIT=0 JSC_useBaselineJIT=0 JSC_useDFGJIT=0 JSC_useFTLJIT=0
'''


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def verify_file(path, entry):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != entry['bytes'] or sha(path) != entry['sha256']:
        raise ValueError(f'Pinned input mismatch: {path.name}')


def external(path):
    path = path.expanduser().absolute()
    if path.is_symlink():
        raise ValueError('Cache roots must not be symlinks')
    path = path.resolve()
    if path == REPOSITORY or REPOSITORY in path.parents:
        raise ValueError('Engine caches must remain outside the repository')
    return path


def own_cache(path):
    path.mkdir(parents=True, exist_ok=True)
    marker = path / '.rmweb-auth-engine-cache'
    if not marker.exists():
        if any(path.iterdir()):
            raise ValueError('Select an empty dedicated engine cache')
        marker.write_bytes(MARKER)
    if marker.is_symlink() or not marker.is_file() or marker.read_bytes() != MARKER:
        raise ValueError('Invalid engine cache marker')
    # Verified archive/source extractions live only inside the dedicated volume.
    # Host output trees contain plain files; reject redirected writes throughout.
    if any(p.is_symlink() for p in path.rglob('*')):
        raise ValueError('Engine cache write paths must not contain symlinks')


def download_inputs(cache, input_cache, lock):
    for relative, entry in lock['files'].items():
        target = cache / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            existing = input_cache / relative if input_cache else None
            if existing is not None and existing.exists():
                if input_cache not in existing.resolve().parents:
                    raise ValueError('Input cache file escapes its root')
                verify_file(existing, entry)
                with target.open('xb') as output, existing.open('rb') as source:
                    shutil.copyfileobj(source, output)
            else:
                with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as output:
                    temporary = Path(output.name)
                    try:
                        with urllib.request.urlopen(entry['url'], timeout=60) as source:
                            remaining = entry['bytes'] + 1
                            while remaining:
                                block = source.read(min(1024 * 1024, remaining))
                                if not block:
                                    break
                                output.write(block)
                                remaining -= len(block)
                        output.flush()
                        verify_file(temporary, entry)
                        temporary.replace(target)
                    finally:
                        temporary.unlink(missing_ok=True)
        verify_file(target, entry)


def prepare_headers(cache):
    headers = cache / 'headers'
    (headers / 'sources').mkdir(parents=True, exist_ok=True)
    for name in ('wpewebkit-2.48.5.tar.xz', 'libsoup-3.6.0.tar.xz', 'libwpe-1.16.2.tar.xz', 'libxkbcommon-1.7.0.tar.xz'):
        destination = headers / 'sources' / name
        if not destination.exists():
            shutil.copyfile(cache / 'inputs' / name, destination)
    subprocess.run([sys.executable, str(REPOSITORY / 'scripts/prepare-app-headers.py'), '--cache', str(headers)], check=True)
    # The dependency profile below rejects changed SDK/header inputs for a reused
    # volume. Stable generated-header times avoid rebuilding correct objects when
    # the identical headers are copied into a fresh disposable SDK container.
    for path in (headers / 'stage/usr').rglob('*'):
        if path.is_file():
            os.utime(path, (0, 0))


def contained_files(root, source_links=False):
    """Dereference only regular files whose resolved target stays in this tree."""
    root = root.resolve()
    for path in sorted(root.rglob('*')):
        resolved = path.resolve()
        if root not in resolved.parents:
            raise ValueError('Runtime symlink escapes the verified input')
        if path.is_symlink() and resolved.is_dir():
            raise ValueError('Runtime directory symlinks are unsupported')
        if source_links and path.is_symlink():
            # The pinned source archive contains an unused, dangling Skia Cargo
            # link. Preserve contained source links exactly; runtime staging
            # never enables this branch and still requires complete plain files.
            yield path.relative_to(root), path
            continue
        if resolved.is_dir():
            continue
        if not resolved.is_file() or not stat.S_ISREG(resolved.stat().st_mode):
            raise ValueError('Runtime contains a non-regular file')
        yield path.relative_to(root), resolved


def copy_plain(root, destination, accept=lambda path: True):
    for relative, source in contained_files(root):
        if not accept(relative):
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(0o755 if source.stat().st_mode & 0o111 else 0o644)


def file_map(root):
    result = {}
    for relative, source in contained_files(root):
        path = root / relative
        if path.is_symlink():
            raise ValueError('Published artifacts must be plain files')
        result[str(relative)] = {'sha256': sha(source), 'bytes': source.stat().st_size,
                                 'mode': source.stat().st_mode & 0o777}
    return result


def sync_source(canonical, destination):
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError('Source destination must be an owned directory')
    expected = dict(contained_files(canonical, source_links=True))
    destination.mkdir(exist_ok=True)
    for relative, source in list(contained_files(destination, source_links=True)):
        if relative in expected:
            continue
        # Python generators create bytecode next to their sources. Remove only
        # an identifiable cache for a canonical .py input before any next build.
        python_source = relative.parent.parent / (relative.name.split('.')[0] + '.py')
        if relative.parent.name == '__pycache__' and relative.suffix == '.pyc' and python_source in expected:
            (destination / relative).unlink()
        else:
            raise ValueError('Unexpected source file in private build tree: ' + str(relative))
    for relative, source in expected.items():
        target = destination / relative
        if source.is_symlink():
            link = os.readlink(source)
            if not target.is_symlink() or os.readlink(target) != link:
                target.unlink(missing_ok=True)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(link)
            continue
        if target.is_symlink():
            target.unlink()
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or sha(target) != sha(source):
            shutil.copy2(source, target)
            # A reverted patch can restore old archive timestamps. Mark changed
            # content fresh so Ninja cannot retain an object from different code.
            os.utime(target, None)


def publish_artifacts(output, destination):
    previous = destination.with_name('.artifacts.previous')
    for path in (destination, previous):
        if path.is_symlink() or (path.exists() and any(p.is_symlink() for p in path.rglob('*'))):
            raise ValueError('Refusing redirected artifact output')
    if not destination.exists() and previous.exists():
        previous.replace(destination)
    if previous.exists():
        shutil.rmtree(previous)
    if destination.exists():
        destination.replace(previous)
    try:
        output.replace(destination)
    except OSError:
        if previous.exists():
            previous.replace(destination)
        raise
    # Retain the previous complete result until the next successful invocation.


def verify_dependency_profile(root, expected):
    profile = root / '.dependency-profile.json'
    if profile.is_symlink():
        raise ValueError('Refusing redirected dependency profile')
    if profile.exists():
        if not profile.is_file() or json.loads(profile.read_text()) != expected:
            raise ValueError('Changed SDK/dependency inputs require a fresh build volume')
    else:
        if (root / 'configure-webauthn-on').exists():
            raise ValueError('Existing build objects have no verified dependency profile')
        profile.write_text(json.dumps(expected, sort_keys=True) + '\n')


def own_build_volume(root):
    marker = root / '.purpose'
    if marker.exists():
        if marker.is_symlink() or not marker.is_file() or marker.read_text() != VOLUME_MARKER:
            # Earlier markers are not migrated: use a fresh volume so unrelated
            # or downstream-owned build objects are never silently adopted.
            raise ValueError('Unrecognized build volume; use a fresh dedicated rmweb build volume')
    else:
        if any(root.iterdir()):
            raise ValueError('Build volume must be empty or owned')
        marker.write_text(VOLUME_MARKER)


def prepare_container():
    root = Path('/build')
    own_build_volume(root)
    work = Path('/work')
    lock = json.loads(LOCK.read_text())
    for relative, entry in lock['files'].items():
        verify_file(work / relative, entry)
    plan = json.loads((work / 'build-plan.json').read_text())
    dependency_profile = {'sdk': plan['sdk'], 'inputs': sha(LOCK),
                          'headers': sha(REPOSITORY / 'scripts/prepare-app-headers.py')}
    verify_dependency_profile(root, dependency_profile)
    runtime = root / 'runtime'
    with tempfile.TemporaryDirectory(prefix='base-runtime-', dir=root) as temporary:
        canonical_runtime = Path(temporary) / 'runtime'
        with tarfile.open(work / 'inputs/rmweb-0.9.1.tar.gz') as archive:
            archive.extractall(canonical_runtime, filter='data')
        if runtime.is_symlink() or (canonical_runtime / 'VERSION').read_text().strip() != '0.9.1':
            raise ValueError('Unexpected private base runtime')
        if runtime.exists():
            shutil.rmtree(runtime)
        canonical_runtime.replace(runtime)
    # Extract canonical source each time, apply the reviewed patch, then update
    # changed files only. Unchanged object inputs retain their build timestamps.
    with tempfile.TemporaryDirectory(prefix='engine-source-', dir=root) as temporary:
        with tarfile.open(work / 'inputs/wpewebkit-2.48.5.tar.xz') as archive:
            archive.extractall(temporary, filter='data')
        canonical = Path(temporary) / ENGINE
        subprocess.run(['git', 'apply', '--check', str(PATCH)], cwd=canonical, check=True)
        subprocess.run(['git', 'apply', str(PATCH)], cwd=canonical, check=True)
        receipt = json.loads(SOURCE_FILES.read_text())
        files = receipt.get('files', receipt)
        for relative, entry in files.items():
            expected = entry.get('sha256') if isinstance(entry, dict) else entry
            if sha(canonical / relative) != expected:
                raise ValueError('Provider source receipt mismatch')
        sync_source(canonical, root / ENGINE)
    applied = root / 'provider-applied.patch'
    if applied.is_symlink():
        raise ValueError('Refusing redirected provider receipt')
    shutil.copyfile(PATCH, applied)


def stage_container():
    work, build = Path('/work'), Path('/build')
    plan = json.loads((work / 'build-plan.json').read_text())
    for name, expected in plan['buildInputs'].items():
        if sha(REPOSITORY / name) != expected:
            raise ValueError('Engine recipe or provider changed during the build')
    with tempfile.TemporaryDirectory(prefix='artifacts-', dir=work) as temporary:
        output = Path(temporary)
        runtime, devel = output / 'runtime', output / 'devel'
        runtime.mkdir(); devel.mkdir()
        allowed = {'lib', 'libexec', 'share', 'licenses', 'VERSION'}
        copy_plain(build / 'runtime', runtime, lambda p: p.parts[0] in allowed)
        copy_plain(work / 'headers/stage/usr', devel)
        installed = build / 'auth-runtime-stage/usr'
        # Installation provides all new WPE libraries, injected bundle and helpers.
        copy_plain(installed, runtime, lambda p: p.parts[0] in {'lib', 'libexec', 'share'}
                   and not (p.parts[:2] == ('lib', 'pkgconfig')) and p.suffix != '.a')
        copy_plain(installed, devel, lambda p: p.parts[0] == 'include' or p.parts[:2] == ('lib', 'pkgconfig'))
        (runtime / 'rmweb-env.sh').write_text(ENVIRONMENT)
        (runtime / 'rmweb-env.sh').chmod(0o644)
        (runtime / 'VERSION').write_text(RUNTIME_VERSION)
        licenses = runtime / 'licenses'
        licenses.mkdir(exist_ok=True)
        shutil.copyfile(PATCH, licenses / PATCH.name)
        shutil.copyfile(SOURCE_FILES, licenses / 'provider-source-files.json')
        shutil.copyfile(work / 'inputs/wpewebkit-2.48.5.tar.xz', licenses / 'wpewebkit-2.48.5.tar.xz')
        recipe_names = list(plan['buildInputs'])
        for name in recipe_names:
            target = licenses / 'build-recipe' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPOSITORY / name, target)
        for path in (build / ENGINE / 'Source').rglob('*'):
            if path.is_file() and path.name.upper().startswith(('LICENSE', 'COPYING')):
                relative = path.relative_to(build / ENGINE)
                target = licenses / 'wpe-source' / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        provenance = {**plan, 'sourceURL': 'https://wpewebkit.org/releases/wpewebkit-2.48.5.tar.xz',
                      'supportRuntimeURL': 'https://github.com/exp78/rmweb/releases/download/v0.9.1/rmweb-0.9.1.tar.gz'}
        (licenses / 'engine-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
        for name in ('lib/libWPEWebKit-2.0.so.1', 'libexec/wpe-webkit-2.0/WPEWebProcess',
                     'libexec/wpe-webkit-2.0/WPENetworkProcess', 'libexec/wpe-webkit-2.0/WPEGPUProcess'):
            if not (runtime / name).is_file():
                raise ValueError('Missing matching engine component: ' + name)
        for root in (runtime, devel):
            for path in root.rglob('*'):
                if path.is_file():
                    path.chmod(0o755 if path.stat().st_mode & 0o111 else 0o644)
        manifest = {'schemaVersion': 1, 'purpose': 'auth-engine', 'firmware': '3.28.0.172',
                    'sdk': plan['sdk'], 'upstream': plan['upstream'],
                    'patch': {'path': 'runtime/licenses/' + PATCH.name, 'sha256': plan['patchSha256']},
                    'runtimeFiles': file_map(runtime), 'develFiles': file_map(devel)}
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        publish_artifacts(output, work / 'artifacts')


def main():
    if sys.argv[1:] == ['--container-prepare']:
        prepare_container(); return
    if sys.argv[1:] == ['--container-stage']:
        stage_container(); return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--input-cache', type=Path)
    parser.add_argument('--sdk-image', default='rmweb-app-sdk:3.28.0.172')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--build-volume', help='Reuse only a volume with the exact owned-purpose marker')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 12:
        parser.error('jobs must be between1 and12')
    cache = external(args.cache); own_cache(cache)
    with (cache / '.build.lock').open('a') as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lock = json.loads(LOCK.read_text())
        download_inputs(cache, external(args.input_cache) if args.input_cache else None, lock)
        prepare_headers(cache)
        image = json.loads(subprocess.check_output(['docker', 'image', 'inspect', args.sdk_image]))[0]
        if image['Os'] != 'linux' or image['Architecture'] != 'arm64':
            raise ValueError('Expected an ARM64 Linux SDK image')
        plan = {'sdk': {'imageId': image['Id'], 'sha256': lock['sdkSha256'], 'qtVersion': lock['qtVersion']},
                'upstream': {'version': '2.48.5', 'sourceSha256': lock['files']['inputs/wpewebkit-2.48.5.tar.xz']['sha256'],
                             'runtimeSha256': lock['files']['inputs/rmweb-0.9.1.tar.gz']['sha256']},
                'patchSha256': sha(PATCH), 'recipeSha256': sha(Path(__file__)), 'inputLockSha256': sha(LOCK)}
        names = ['scripts/build-auth-engine.py', 'scripts/build-auth-engine-container.sh',
                 'scripts/prepare-app-headers.py', 'scripts/app_only_cache.py',
                 'toolchain/Dockerfile.app-only-sdk-3.28',
                 str(LOCK.relative_to(REPOSITORY)), str(PATCH.relative_to(REPOSITORY)),
                 str(SOURCE_FILES.relative_to(REPOSITORY))]
        plan['buildInputs'] = {name: sha(REPOSITORY / name) for name in names}
        (cache / 'build-plan.json').write_text(json.dumps(plan, indent=2) + '\n')
        volume = args.build_volume or 'rmweb-auth-engine-' + hashlib.sha256(str(cache).encode()).hexdigest()[:16]
        if not volume or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-' for c in volume):
            raise ValueError('Invalid dedicated Docker volume name')
        subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--platform', 'linux/arm64',
                        '--cpus', str(min(args.jobs + 2, 14)), '--memory', '40g',
                        '--mount', f'type=bind,src={REPOSITORY},dst=/src,readonly',
                        '--mount', f'type=bind,src={cache},dst=/work',
                        '--mount', f'type=volume,src={volume},dst=/build',
                        image['Id'], 'bash', '/src/scripts/build-auth-engine-container.sh', str(args.jobs)], check=True)
        print('Engine artifacts:', cache / 'artifacts')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
