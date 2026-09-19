#!/usr/bin/env python3
"""Export the verified helper binary and complete source/license materials."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile

PIN = "a6bdb700918f4c8c06c52d41f4d2d9e79888c5ad"
SDK_SHA = "65e5b98f9f7c83d857c5c720f7f6cb2d61fe20f4513dec023ad00aac27c85ae4"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def record(path):
    return {"sha256":sha(path), "bytes":path.stat().st_size, "mode":f"{stat.S_IMODE(path.stat().st_mode):04o}"}


def owned_directory(path):
    if not path.is_absolute() or path.is_symlink() or not path.is_dir() or path.stat().st_uid != os.getuid():
        raise ValueError("expected an owned absolute source/cache directory")
    for parent in path.parents:
        if parent.is_symlink():
            raise ValueError("symlink directory component")


def export(stage, binary, output, image_id, rmweb_revision, cargo):
    owned_directory(stage)
    if output.exists() or output.is_symlink():
        raise ValueError("artifact output must not already exist")
    owned_directory(output.parent)
    if not re.fullmatch(r"sha256:[0-9a-f]{64}",image_id) or not re.fullmatch(r"[0-9a-f]{40}",rmweb_revision):
        raise ValueError("invalid build provenance")
    receipt = json.loads((stage / "source-receipt.json").read_text())
    if receipt["libraryCommit"] != PIN:
        raise ValueError("unexpected library revision")
    for name, digest in receipt["files"].items():
        path = stage / name
        if path.is_symlink() or not path.is_file() or sha(path) != digest:
            raise ValueError("staged source changed after preparation")
    if binary.is_symlink() or not binary.is_file() or binary.stat().st_uid != os.getuid():
        raise ValueError("expected owned regular helper binary")
    if binary.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("helper binary exceeds size bound")
    with binary.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20],"little") != 183:
        raise ValueError("helper must be a little-endian AArch64 ELF")
    before_binary = sha(binary)
    # A fresh directory ensures Cargo cannot reuse editable vendored sources.
    dependencies = stage / "dependencies"
    if dependencies.exists() or dependencies.is_symlink():
        raise ValueError("use a freshly prepared stage for export")
    config = subprocess.check_output([cargo,"vendor","--locked",str(dependencies)],cwd=stage,text=True)
    config = config.replace(str(dependencies),"dependencies")
    (stage / ".cargo").mkdir()
    (stage / ".cargo/config.toml").write_text(config)
    temporary = Path(tempfile.mkdtemp(prefix=".helper-artifacts-",dir=output.parent))
    try:
        licenses = temporary / "licenses"
        licenses.mkdir()
        shutil.copy2(binary,temporary / "rmweb-auth-passkey")
        (temporary / "rmweb-auth-passkey").chmod(0o755)
        for source,destination in [("vendor/libwebauthn-COPYING","COPYING-libwebauthn"),
                                   ("vendor/LICENSE-rmweb","LICENSE-rmweb"),
                                   ("patches/libwebauthn-buffered-response.patch","libwebauthn-buffered-response.patch"),
                                   ("README.md","README-helper.md")]:
            shutil.copy2(stage/source,licenses/destination)
        (licenses/"NOTICE.txt").write_text("""rmweb phone passkey helper

Statically linked libwebauthn v0.9.0 is LGPL-2.1-or-later, revision
"""+PIN+""", with the supplied buffered-response patch.
helper-source.tar.gz contains complete helper and patched dependency source,
all Cargo.lock dependency sources/license notices, the MPL-2.0 Public Suffix
List source/notice, and build/relink scripts. Its .cargo/config.toml selects
bundled crates. Unpack, modify the desired source, and run build-sdk.sh with
Rust1.90 and the official PaperPro3.28 SDK. No signature check prevents use
of a rebuilt helper. Runtime system libraries/SDK are supplied separately.
Public Suffix List terms: https://mozilla.org/MPL/2.0/.
""")
        source_files = sorted(path for path in stage.rglob("*") if path.is_file())
        if any(path.is_symlink() for path in stage.rglob("*")):
            raise ValueError("source archive may not contain symlinks")
        with (licenses/"helper-source.tar.gz").open("wb") as raw:
            with gzip.GzipFile(filename="",mode="wb",fileobj=raw,mtime=0) as compressed:
                with tarfile.open(fileobj=compressed,mode="w") as archive:
                    for path in source_files:
                        info = archive.gettarinfo(str(path),arcname="auth-passkey-helper/"+str(path.relative_to(stage)))
                        info.uid=info.gid=0;info.uname=info.gname="";info.mtime=0
                        with path.open("rb") as data:archive.addfile(info,data)
        for path in licenses.iterdir():path.chmod(0o644)
        if before_binary != sha(binary) or before_binary != sha(temporary/"rmweb-auth-passkey"):
            raise ValueError("helper changed during export")
        for name,digest in receipt["files"].items():
            if sha(stage/name) != digest:raise ValueError("source changed during export")
        manifest = {"schemaVersion":1,"binary":{"path":"rmweb-auth-passkey",**record(temporary/"rmweb-auth-passkey")},
            "sourceRevision":{"rmweb":rmweb_revision,"libwebauthn":PIN,"helperSourceSha256":receipt["helperSourceSha256"]},
            "patchSHA256":sha(stage/"patches/libwebauthn-buffered-response.patch"),"SDKsha256":SDK_SHA,
            "rustVersion":"1.90.0","imageId":image_id,
            "licenses":{str(path.relative_to(temporary)):record(path) for path in sorted(licenses.iterdir())}}
        (temporary/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
        temporary.rename(output)
    finally:
        if temporary.exists():shutil.rmtree(temporary)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage",type=Path,required=True)
    parser.add_argument("--binary",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--image-id",required=True)
    parser.add_argument("--rmweb-revision",required=True)
    parser.add_argument("--cargo",default="cargo")
    args=parser.parse_args()
    export(args.stage,args.binary,args.output,args.image_id,args.rmweb_revision,args.cargo)
    print(args.output)

if __name__ == "__main__":
    try: main()
    except (OSError,ValueError,KeyError,subprocess.CalledProcessError) as error:
        raise SystemExit(f"Helper export failed: {error}")
