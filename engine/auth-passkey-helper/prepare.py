#!/usr/bin/env python3
"""Stage source and the checksum-pinned patched dependency outside the checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import tempfile
import urllib.request

PIN = "a6bdb700918f4c8c06c52d41f4d2d9e79888c5ad"
SHA256 = "9cbd2d5afe03cc33f7bdbb7a9d5c81922d8008e55520beb2f4e6cfe35016bb54"
BYTES = 328132
PSL_SHA256 = "bb3d3bb844f1d172de41f0c5089e7b2b7243b02698e4a64120ffcf49ec84c0ee"
URL = f"https://codeload.github.com/linux-credentials/libwebauthn/tar.gz/{PIN}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    cache = args.cache.absolute()
    repository = Path(__file__).resolve().parents[2]
    if cache == repository or repository in cache.parents:
        raise SystemExit("build cache must remain outside the checkout")
    for item in [cache, *cache.parents]:
        if item.is_symlink():
            raise SystemExit("cache must not contain symlink components")
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = cache.stat()
    if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
        raise SystemExit("cache must be owned and private (0700)")
    source = Path(__file__).resolve().parent
    if hashlib.sha256((source / "vendor/public_suffix_list.dat").read_bytes()).hexdigest() != PSL_SHA256:
        raise SystemExit("public suffix list checksum mismatch")
    archive = cache / f"libwebauthn-{PIN}.tar.gz"
    if archive.exists() or archive.is_symlink():
        info = archive.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_size != BYTES:
            raise SystemExit("source archive is not the expected regular file")
        data = archive.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=30) as response:
            data = response.read(BYTES + 1)
        if len(data) != BYTES or hashlib.sha256(data).hexdigest() != SHA256:
            raise SystemExit("source archive checksum mismatch")
        with archive.open("xb") as output:
            output.write(data)
        archive.chmod(0o600)
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit("source archive checksum mismatch")
    stage = Path(tempfile.mkdtemp(prefix="helper-", dir=cache))
    for name in [".gitignore", "Cargo.toml", "Cargo.lock", "prepare.py", "package.py", "src", "tests", "patches", "vendor", "README.md", "build-sdk.sh", "sdk-linker.sh"]:
        item = source / name
        if not item.exists():
            if name in {"Cargo.lock", "README.md"}:
                continue
            raise SystemExit(f"source missing: {name}")
        if item.is_dir():
            shutil.copytree(item, stage / name, ignore=shutil.ignore_patterns("libwebauthn"))
        else:
            shutil.copy2(item, stage / name)
    unpack = stage / "unpack"
    unpack.mkdir()
    with tarfile.open(archive, "r:gz") as bundle:
        for entry in bundle.getmembers():
            if not (entry.isfile() or entry.isdir()) or entry.name.startswith("/") or ".." in Path(entry.name).parts:
                raise SystemExit("unexpected archive member")
        bundle.extractall(unpack, filter="data")
    upstream = unpack / f"libwebauthn-{PIN}"
    upstream.rename(stage / "vendor/libwebauthn")
    unpack.rmdir()
    subprocess.run(["patch", "--batch", "--forward", "-p1", "-i", str(stage / "patches/libwebauthn-buffered-response.patch")],
                   cwd=stage / "vendor/libwebauthn", check=True, stdout=subprocess.DEVNULL)
    files = {str(item.relative_to(stage)): hashlib.sha256(item.read_bytes()).hexdigest()
             for item in stage.rglob("*") if item.is_file()}
    helper_files = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(source.rglob("*")) if p.is_file() and "__pycache__" not in p.parts
                    and "target" not in p.relative_to(source).parts and "libwebauthn" not in p.relative_to(source).parts}
    helper_sha = hashlib.sha256(json.dumps(helper_files,sort_keys=True,separators=(",", ":")).encode()).hexdigest()
    (stage / "source-receipt.json").write_text(json.dumps({"libraryCommit": PIN, "archiveSha256": SHA256, "helperSourceSha256": helper_sha,
        "archiveUrl": URL, "files": files}, indent=2) + "\n")
    print(stage)


if __name__ == "__main__":
    main()
