#!/usr/bin/env python3
"""Tests for the TLS harness, separate from WebKit/provider qualification."""

import importlib.util
import os
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load("tls_runner", "run-https-cases.py")
server_module = load("tls_server", "https-server.py")


class TlsServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.ca, leaf, key = runner.certificates(Path(cls.temp.name))
        cls.server = server_module.Server(leaf, key, port=0)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temp.cleanup()

    def connect(self, context, hostname="login.example.com"):
        raw = socket.create_connection(self.server.server_address, timeout=3)
        try:
            return context.wrap_socket(raw, server_hostname=hostname)
        except Exception:
            raw.close()
            raise

    def request(self, path):
        context = ssl.create_default_context(cafile=str(self.ca))
        with self.connect(context) as connection:
            connection.sendall(f"GET {path} HTTP/1.0\r\nHost: login.example.com\r\n\r\n".encode("ascii"))
            response = bytearray()
            while data := connection.recv(4096):
                response.extend(data)
            return bytes(response)

    def test_trusted_parent_and_child_are_actual_fixed_html(self):
        self.assertIn(b'<iframe id="child" src="/child">', self.request("/parent"))
        self.assertIn(b"200 OK", self.request("/child"))

    def test_untrusted_certificate_is_rejected(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.connect(ssl.create_default_context())

    def test_wrong_hostname_is_rejected_even_with_ca(self):
        context = ssl.create_default_context(cafile=str(self.ca))
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.connect(context, "unrelated.example.com")

    def test_unknown_path_never_reads_files(self):
        response = self.request("/../../etc/passwd")
        self.assertIn(b"404 Not Found", response)
        self.assertEqual(response.split(b"\r\n\r\n", 1)[1], b"")

    def test_test_trust_environment_removes_proxy_and_other_overrides(self):
        from unittest.mock import patch
        with patch.dict(os.environ, {"https_proxy": "http://unused.invalid", "ALL_PROXY": "x",
                                     "SSL_CERT_DIR": "/unrelated", "SSL_CERT_FILE": "/old"}):
            env = runner.fixture_environment(self.ca)
        self.assertEqual(env["SSL_CERT_FILE"], str(self.ca))
        self.assertEqual(env["NO_PROXY"], "login.example.com")
        self.assertNotIn("https_proxy", env)
        self.assertNotIn("ALL_PROXY", env)
        self.assertNotIn("SSL_CERT_DIR", env)


def isolated():
    try:
        runner.require_isolation()
        return True
    except (RuntimeError, OSError):
        return False


@unittest.skipUnless(isolated(), "runner process checks require the documented isolated Linux container")
class RunnerProcessTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def start(self, mode):
        executable = self.root / "fake-fixture"
        # This child tests the harness only; it makes a real, verified TLS
        # request and never impersonates WebKit or qualifies the provider.
        executable.write_text(f'''#!{sys.executable}
import os, pathlib, socket, ssl, sys, time
root = pathlib.Path({str(self.root)!r})
assert {{p.name for p in pathlib.Path('/sys/class/net').iterdir()}} == {{'lo', 'wpe-test0'}}
assert len(pathlib.Path('/proc/net/route').read_text().splitlines()) == 1
assert socket.getaddrinfo('login.example.com', 443, socket.AF_INET, socket.SOCK_STREAM,
                          flags=socket.AI_ADDRCONFIG)[0][4][0] == '127.0.0.1'
ca = pathlib.Path(os.environ["SSL_CERT_FILE"])
assert ca.parent.stat().st_mode & 0o077 == 0
context = ssl.create_default_context()
with socket.create_connection(("login.example.com", 443), timeout=3) as raw:
    with context.wrap_socket(raw, server_hostname="login.example.com") as connection:
        connection.sendall(b"GET /parent HTTP/1.0\\r\\nHost: login.example.com\\r\\n\\r\\n")
        assert b"200 OK" in connection.recv(4096)
with (root / "cases").open("a") as output:
    output.write(sys.argv[1] + "\\n")
(root / "ca-path").write_text(str(ca))
(root / "child-pid").write_text(str(os.getpid()))
if {mode!r} == "failure": sys.exit(3)
if {mode!r} == "wait": time.sleep(60)
''', encoding="utf-8")
        executable.chmod(0o700)
        env = os.environ.copy()
        env["TMPDIR"] = str(self.root)
        return subprocess.Popen([sys.executable, str(HERE / "run-https-cases.py"), str(executable)],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def assert_clean(self):
        self.assertEqual({p.name for p in Path('/sys/class/net').iterdir()}, {'lo'})
        ca = Path((self.root / "ca-path").read_text())
        self.assertFalse(ca.parent.exists())
        self.assertFalse(list(self.root.glob("rmweb-provider-tls-*")))
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", 443), timeout=0.2)

    def test_runs_exact_two_cases_and_removes_private_material(self):
        process = self.start("success")
        out, err = process.communicate(timeout=40)
        self.assertEqual(process.returncode, 0, (out, err))
        self.assertEqual((self.root / "cases").read_text().splitlines(), list(runner.CASES))
        self.assert_clean()

    def test_family_specific_resolver_is_enabled_only_in_fixture(self):
        with self.assertRaises(socket.gaierror):
            socket.getaddrinfo(runner.HOST, 443, socket.AF_INET, socket.SOCK_STREAM,
                               flags=socket.AI_ADDRCONFIG)
        with runner.resolver_interface():
            addresses = socket.getaddrinfo(runner.HOST, 443, socket.AF_INET, socket.SOCK_STREAM,
                                           flags=socket.AI_ADDRCONFIG)
            self.assertEqual({item[4][0] for item in addresses}, {'127.0.0.1'})
        self.assertEqual({p.name for p in Path('/sys/class/net').iterdir()}, {'lo'})

    def test_failed_ack_after_interface_creation_removes_owned_interface(self):
        from unittest.mock import patch
        original = runner.netlink_request

        def fail_after_create(kind, flags, payload):
            original(kind, flags, payload)
            if kind == 16:
                raise TimeoutError('injected lost create acknowledgement')

        with patch.object(runner, 'netlink_request', side_effect=fail_after_create):
            with self.assertRaises(TimeoutError):
                with runner.resolver_interface():
                    self.fail('unexpected fixture entry')
        self.assertEqual({p.name for p in Path('/sys/class/net').iterdir()}, {'lo'})

    def test_sigterm_after_interface_creation_removes_owned_interface(self):
        from unittest.mock import patch
        original = runner.netlink_request
        previous = signal.getsignal(signal.SIGTERM)

        def terminate(_signal, _frame):
            raise KeyboardInterrupt

        def terminate_after_create(kind, flags, payload):
            original(kind, flags, payload)
            if kind == 16:
                os.kill(os.getpid(), signal.SIGTERM)

        signal.signal(signal.SIGTERM, terminate)
        try:
            with patch.object(runner, 'netlink_request', side_effect=terminate_after_create):
                with self.assertRaises(KeyboardInterrupt):
                    with runner.resolver_interface():
                        self.fail('unexpected fixture entry')
        finally:
            signal.signal(signal.SIGTERM, previous)
        self.assertEqual({p.name for p in Path('/sys/class/net').iterdir()}, {'lo'})

    def test_first_failure_stops_and_cleans_up(self):
        process = self.start("failure")
        process.communicate(timeout=40)
        self.assertEqual(process.returncode, 1)
        self.assertEqual((self.root / "cases").read_text().splitlines(), [runner.CASES[0]])
        self.assert_clean()

    def test_outer_sigterm_cleans_server_and_running_fixture(self):
        process = self.start("wait")
        try:
            deadline = time.monotonic() + 15
            while not (self.root / "child-pid").exists() and time.monotonic() < deadline:
                if process.poll() is not None:
                    self.fail("runner exited before starting the fixture")
                time.sleep(0.02)
            self.assertTrue((self.root / "child-pid").exists())
            child_pid = int((self.root / "child-pid").read_text())
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=8)
            self.assertEqual(process.returncode, 1)
            with self.assertRaises(ProcessLookupError):
                os.kill(child_pid, 0)
            self.assert_clean()
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main()
