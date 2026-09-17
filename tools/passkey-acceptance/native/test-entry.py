#!/usr/bin/env python3
"""Run the actual namespace-entry fixture against the isolated acceptance entry."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--without-mount-capability", action="store_true")
    args = parser.parse_args()
    if not Path("/.dockerenv").is_file() or os.geteuid() != 0:
        parser.error("run only inside a disposable root-owned Docker container")
    repo = Path(__file__).resolve().parents[3]
    command = [sys.executable, str(repo / "tests/auth_entry_test.py"), str(args.binary),
               "--application-root", "/opt/rmweb-passkey-acceptance-fixture"]
    if args.without_mount_capability:
        command.append("--without-mount-capability")
    subprocess.run(command, check=True, timeout=150)


if __name__ == "__main__":
    main()
