#!/usr/bin/env python3
"""Run two real-HTTPS iframe cases in a disposable, network-disabled container."""

import argparse
import errno
import os
import signal
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from contextlib import contextmanager


CASES = ("child-https-success", "child-https-detach")
HOST = "login.example.com"
RESOLVER_INTERFACE = "wpe-test0"


def require_isolation():
    if sys.platform != "linux" or os.geteuid() != 0:
        raise RuntimeError("use the disposable Linux test container")
    if {path.name for path in Path("/sys/class/net").iterdir()} != {"lo"}:
        raise RuntimeError("the test container must have networking disabled")
    addresses = {item[4][0] for item in socket.getaddrinfo(HOST, 443, type=socket.SOCK_STREAM)}
    if addresses != {"127.0.0.1"}:
        raise RuntimeError("map the fixed test hostname to container loopback")


def netlink_attribute(kind, value):
    data = struct.pack("=HH", len(value) + 4, kind) + value
    return data + bytes((-len(data)) % 4)


def netlink_request(kind, flags, payload):
    # Linux rtnetlink, confined to the disposable --network none namespace.
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE) as channel:
        channel.settimeout(2)
        channel.bind((0, 0))
        message = struct.pack("=IHHII", len(payload) + 16, kind, flags | 5, 1, 0) + payload
        channel.sendto(message, (0, 0))  # NLM_F_REQUEST | NLM_F_ACK
        reply = channel.recv(4096)
        if len(reply) < 20 or struct.unpack_from("=H", reply, 4)[0] != 2:
            raise RuntimeError("unexpected fixture network acknowledgement")
        error = struct.unpack_from("=i", reply, 16)[0]
        if error:
            raise OSError(-error, os.strerror(-error))


@contextmanager
def resolver_interface():
    # glibc's AF_INET + AI_ADDRCONFIG rejects loopback-only namespaces. A
    # dummy TEST-NET address enables that check without any external route.
    require_isolation()
    index = None
    create_attempted = False
    try:
        link = struct.pack("=BBHiII", 0, 0, 0, 0, 1, 1)  # IFF_UP
        link += netlink_attribute(3, RESOLVER_INTERFACE.encode("ascii") + b"\0")
        link += netlink_attribute(18, netlink_attribute(1, b"dummy\0"))
        create_attempted = True
        try:
            netlink_request(16, 0x600, link)  # RTM_NEWLINK, CREATE | EXCL
        except OSError as error:
            if error.errno == errno.EEXIST:
                create_attempted = False  # Never adopt an existing interface.
            raise
        index = socket.if_nametoindex(RESOLVER_INTERFACE)
        address = socket.inet_aton("192.0.2.1")
        payload = struct.pack("=BBBBI", socket.AF_INET, 32, 0, 253, index)
        payload += netlink_attribute(1, address) + netlink_attribute(2, address)
        netlink_request(20, 0x600, payload)  # RTM_NEWADDR, link scope
        if {p.name for p in Path("/sys/class/net").iterdir()} != {"lo", RESOLVER_INTERFACE}:
            raise RuntimeError("unexpected fixture interface")
        if len(Path("/proc/net/route").read_text().splitlines()) != 1:
            raise RuntimeError("the fixture must have no IPv4 external routes")
        addresses = {item[4][0] for item in socket.getaddrinfo(
            HOST, 443, socket.AF_INET, socket.SOCK_STREAM, flags=socket.AI_ADDRCONFIG)}
        if addresses != {"127.0.0.1"}:
            raise RuntimeError("the fixture resolver must remain on loopback")
        yield
    finally:
        # A signal or lost ACK may arrive after NEWLINK succeeded but before
        # index readback. The fixed name was absent before our exclusive create.
        if index is None and create_attempted:
            try:
                index = socket.if_nametoindex(RESOLVER_INTERFACE)
            except OSError:
                pass
        if index is not None:
            if socket.if_nametoindex(RESOLVER_INTERFACE) != index:
                raise RuntimeError("the owned fixture interface changed")
            netlink_request(17, 0, struct.pack("=BBHiII", 0, 0, 0, index, 0, 0))


def certificates(directory):
    """Generate a disposable CA and a separately signed, hostname-bound leaf."""
    ca = directory / "ca.pem"
    leaf = directory / "server.pem"
    key = directory / "server.key"
    extensions = directory / "server.ext"
    extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:login.example.com\n",
        encoding="ascii",
    )
    commands = [
        ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
         "-subj", "/CN=Disposable WebAuthn Test CA", "-keyout", str(directory / "ca.key"),
         "-out", str(ca), "-addext", "basicConstraints=critical,CA:TRUE",
         "-addext", "keyUsage=critical,keyCertSign,cRLSign"],
        ["req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256",
         "-subj", "/CN=login.example.com", "-keyout", str(key),
         "-out", str(directory / "server.csr")],
        ["x509", "-req", "-in", str(directory / "server.csr"), "-CA", str(ca),
         "-CAkey", str(directory / "ca.key"), "-CAcreateserial", "-days", "1",
         "-sha256", "-extfile", str(extensions), "-out", str(leaf)],
    ]
    # The browser runner also replaces the container's dynamic loader. Match
    # its SDK OpenSSL executable and libraries when that environment is active.
    env = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "OPENSSL_CONF", "OPENSSL_MODULES"):
        env.pop(name, None)
    sysroot = env.get("SDKTARGETSYSROOT")
    if sysroot:
        openssl = str(Path(sysroot) / "usr/bin/openssl")
        env["LD_LIBRARY_PATH"] = str(Path(sysroot) / "usr/lib")
    else:
        openssl = shutil.which("openssl")
    if not openssl or not os.access(openssl, os.X_OK):
        raise RuntimeError("the test container requires matching OpenSSL")
    env["OPENSSL_CONF"] = os.devnull
    for command in commands:
        subprocess.run([openssl, *command], env=env, check=True, timeout=10,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    return ca, leaf, key


def stop_group(process):
    # Every child starts a new process group. Include WPE helper descendants
    # even when the fixture's main process has already exited.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=2)


def wait_ready(process, marker):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("the loopback TLS server exited before readiness")
        if marker.is_file() and marker.read_bytes() == b"ready\n":
            return
        time.sleep(0.02)
    raise RuntimeError("the loopback TLS server did not become ready")


def fixture_environment(ca):
    env = os.environ.copy()
    for name in tuple(env):
        if name.lower().endswith("_proxy") or name in ("SSL_CERT_DIR", "CURL_CA_BUNDLE", "REQUESTS_CA_BUNDLE"):
            env.pop(name)
    env["SSL_CERT_FILE"] = str(ca)
    env["NO_PROXY"] = HOST
    return env


def run_cases(binary):
    require_isolation()
    with resolver_interface(), tempfile.TemporaryDirectory(prefix="rmweb-provider-tls-") as raw:
        directory = Path(raw)
        ca, leaf, key = certificates(directory)
        ready = directory / "ready"
        server = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("https-server.py")), str(leaf), str(key), str(ready)],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            wait_ready(server, ready)
            for case in CASES:
                child = subprocess.Popen([str(binary), case], env=fixture_environment(ca),
                                         stdin=subprocess.DEVNULL, start_new_session=True)
                try:
                    status = child.wait(timeout=15)
                    if status:
                        raise RuntimeError(f"{case} failed with status {status}")
                finally:
                    stop_group(child)
        finally:
            stop_group(server)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("the provider fixture must be an executable file")
    os.umask(0o077)
    # Raise into the same finally blocks on an outer runner's cancellation.
    def terminate(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    try:
        run_cases(binary)
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        print(f"FAIL HTTPS fixture: {type(error).__name__}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
