#!/usr/bin/env python3
"""Loopback TLS route gate only; no phone, helper ceremony, account or assertion."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[3]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value

def main():
    os.umask(0o077)
    if len(sys.argv) != 2:
        return 2
    tls = module('tls_fixture', ROOT / 'tests/auth-webauthn-provider/run-https-cases.py')
    web = module('tls_server', ROOT / 'tests/auth-webauthn-provider/https-server.py')
    web.PAGES = {'/verify': b'<!doctype html><meta charset="utf-8"><body>Disposable route fixture</body>'}
    def cancelled(*_args):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, cancelled)
    try:
        with tls.resolver_interface(), tempfile.TemporaryDirectory(prefix='acceptance-tls-') as raw:
            ca, leaf, key = tls.certificates(Path(raw))
            with web.Server(leaf, key) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    for case in ('reload', 'route', 'origin', 'blank'):
                        child = subprocess.Popen([sys.argv[1], case], env=tls.fixture_environment(ca),
                            stdin=subprocess.DEVNULL, start_new_session=True)
                        try:
                            if child.wait(timeout=15):
                                return 1
                        finally:
                            tls.stop_group(child)
                finally:
                    server.shutdown(); thread.join(timeout=2)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError, KeyboardInterrupt):
        print('acceptance TLS fixture FAIL', file=sys.stderr)
        return 1
if __name__ == '__main__':
    sys.exit(main())
