#!/usr/bin/env python3
"""Prepare a separate temporary AppLoad catalog; never write to a tablet."""
import argparse
import json
from pathlib import Path
import re
import shutil
from urllib.parse import urlsplit


def manifest(origin: str) -> dict:
    url = urlsplit(origin)
    if (url.scheme != "https" or url.path or url.query or url.fragment
            or url.username is not None or url.password is not None
            or not url.hostname or "." not in url.hostname
            or not re.fullmatch(r"[a-z0-9]+(?:[.-][a-z0-9]+)*", url.hostname)
            or re.fullmatch(r"[0-9.]+", url.hostname)
            or url.port == 443
            or origin != "https://" + url.hostname + (f":{url.port}" if url.port else "")):
        raise ValueError("an exact HTTPS test origin with a DNS hostname is required")
    return {
        "name": "Passkey Demo",
        "application": "/home/root/rmweb-passkey-acceptance/launch",
        "workingDirectory": "/home/root/rmweb-passkey-acceptance",
        "args": [origin + "/verify"], "environment": {},
        "qtfb": True, "supportsVirtualKeyboard": True,
        "disablesWindowedMode": True, "supportsRotation": False,
        "aspectRatio": "auto",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="fresh local catalog directory")
    args = parser.parse_args()
    data = manifest(args.origin)
    args.output.mkdir(mode=0o700, parents=False, exist_ok=False)
    (args.output / "external.manifest.json").write_text(json.dumps(data, indent=2) + "\n")
    repo = Path(__file__).resolve().parents[3]
    shutil.copyfile(repo / "device/appload/rmweb/icon.png", args.output / "icon.png")


if __name__ == "__main__":
    main()
