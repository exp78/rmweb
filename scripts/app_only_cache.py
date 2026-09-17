"""Ownership check for this build lane's disposable cache directories."""
from pathlib import Path

MARKER = '.rmweb-app-only-cache'
CONTENTS = b'rmweb-app-only-cache-v1\n'


def reject_redirected_outputs(cache: Path):
    # The verified runtime archive intentionally contains library symlinks.
    # Other cache-owned write trees must not redirect writes outside the cache.
    if any(path.is_symlink() for path in cache.iterdir()):
        raise SystemExit('Refusing a symlink in app-only cache output paths')
    for name in ('sources', 'stage', 'build-app', 'artifacts'):
        tree = cache / name
        if tree.exists() and not tree.is_dir():
            raise SystemExit('An app-only cache directory is not a directory')
        if tree.exists() and any(path.is_symlink() for path in tree.rglob('*')):
            raise SystemExit('Refusing a symlink in app-only cache output paths')


def require_owned_cache(cache: Path):
    cache.mkdir(parents=True, exist_ok=True)
    reject_redirected_outputs(cache)
    marker = cache / MARKER
    if marker.is_symlink():
        raise SystemExit('Refusing a symlinked app-only cache marker')
    if marker.exists():
        if not marker.is_file() or marker.stat().st_size != len(CONTENTS) or marker.read_bytes() != CONTENTS:
            raise SystemExit('Invalid app-only cache ownership marker')
        return
    reserved = ('stage', 'runtime', 'build-app', 'artifacts', 'header-manifest.json')
    if any((cache / name).exists() or (cache / name).is_symlink() for name in reserved):
        raise SystemExit('Refusing occupied, unmarked app-only cache; select a fresh dedicated directory')
    try:
        with marker.open('xb') as output:
            output.write(CONTENTS)
    except FileExistsError:
        # Another creator may race us; never infer ownership from existence alone.
        require_owned_cache(cache)
