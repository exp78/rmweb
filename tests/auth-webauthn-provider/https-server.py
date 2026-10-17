#!/usr/bin/env python3
"""Fixed synthetic pages, served only on container loopback with real TLS."""

import argparse
import http.server
import ssl
from pathlib import Path


PAGES = {
    "/parent": b'<!doctype html><meta charset="utf-8"><title>Parent fixture</title>'
               b'<iframe id="child" src="/child"></iframe>',
    "/child": b'<!doctype html><meta charset="utf-8"><title>Child fixture</title>'
              b'<body>Same-origin WebAuthn fixture</body>',
}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        payload = PAGES.get(self.path)
        self.send_response(200 if payload is not None else 404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(payload or b"")))
        self.end_headers()
        if payload is not None:
            self.wfile.write(payload)

    def log_message(self, *_args):
        pass


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, certificate, key, port=443):
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.context.load_cert_chain(certificate, key)
        super().__init__(("127.0.0.1", port), Handler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(2)
        try:
            return self.context.wrap_socket(connection, server_side=True), address
        except Exception:
            connection.close()
            raise

    def handle_error(self, *_args):
        # The fixture has no diagnostic need for HTTP headers or request data.
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("certificate", type=Path)
    parser.add_argument("key", type=Path)
    parser.add_argument("ready", type=Path)
    args = parser.parse_args()
    with Server(args.certificate, args.key) as server:
        args.ready.write_text("ready\n", encoding="ascii")
        server.serve_forever(poll_interval=0.1)


if __name__ == "__main__":
    main()
