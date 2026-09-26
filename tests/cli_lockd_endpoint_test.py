#!/usr/bin/env python3
"""Selected remote lockd endpoint and indexed query contract."""
import http.server
import json
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import urllib.parse

cli = sys.argv[1]


class Handler(http.server.BaseHTTPRequestHandler):
    requests = []
    failure = False
    metadata_failure = False

    def log_message(self, *_args):
        pass

    def do_POST(self):
        if self.headers.get("Transfer-Encoding") == "chunked":
            parts = []
            while True:
                size = int(self.rfile.readline().strip(), 16)
                if not size:
                    self.rfile.readline()
                    break
                parts.append(self.rfile.read(size))
                self.rfile.read(2)
            body = b"".join(parts)
        else:
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.requests.append((self.path, json.loads(body)))
        response = ({"error": "fixture unavailable"} if self.failure else
                    {"namespace": "cai.sessions",
                     "keys": ["fixture"] if self.metadata_failure else [], "index_seq": 1})
        data = json.dumps(response).encode()
        self.send_response(503 if self.failure else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Correlation-Id", "fixture-lockd")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        data = b'{"error":"metadata unavailable"}'
        self.send_response(503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Correlation-Id", "fixture-metadata")
        self.end_headers()
        self.wfile.write(data)


with tempfile.TemporaryDirectory(dir=pathlib.Path(cli).parent) as directory:
    root = pathlib.Path(directory)
    env = os.environ.copy()
    env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
               NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"
    try:
        for option in ("--list", "--resume"):
            result = subprocess.run([cli, "--lockd", endpoint, option],
                                    cwd=root, env=env, capture_output=True,
                                    text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            assert result.stdout == "No conversations.\n"
            path, request = Handler.requests[-1]
            parsed = urllib.parse.urlsplit(path)
            assert parsed.path == "/v1/query", path
            assert urllib.parse.parse_qs(parsed.query) == {
                "engine": ["index"], "refresh": ["wait_for"]}
            assert request["namespace"] == "cai.sessions", request
            assert request["return"] == "keys"
            assert request["selector"]["eq"]["field"] == (
                "/published" if option == "--list" else "/workspace")
        assert not (root / "state/cai/pouch.key").exists()
        assert not (root / "state/cai/pouch").exists()
        Handler.failure = True
        result = subprocess.run([cli, "--lockd", endpoint, "--list"],
                                cwd=root, env=env, capture_output=True,
                                text=True, timeout=10)
        assert result.returncode != 0 and "503" in result.stderr, result.stderr
        assert "fixture-lockd" in result.stderr, result.stderr
        Handler.failure = False
        Handler.metadata_failure = True
        result = subprocess.run([cli, "--lockd", endpoint, "--list"],
                                cwd=root, env=env, capture_output=True,
                                text=True, timeout=10)
        assert result.returncode != 0 and "503" in result.stderr, result.stderr
        assert "fixture-metadata" in result.stderr, result.stderr
        Handler.metadata_failure = False
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

    # A real TLS handshake verifies the combined CA/client/key bundle wiring.
    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=root, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=CAI test CA", "-keyout", "ca.key", "-out", "ca.crt")
    for name in ("server", "client"):
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={name}",
                "-keyout", f"{name}.key", "-out", f"{name}.csr")
        extension = root / f"{name}.ext"
        extension.write_text("subjectAltName=IP:127.0.0.1\n" if name == "server" else
                             "extendedKeyUsage=clientAuth\n")
        openssl("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.crt",
                "-CAkey", "ca.key", "-CAcreateserial", "-days", "1",
                "-extfile", str(extension), "-out", f"{name}.crt")
    bundle = root / "client.pem"
    bundle.write_bytes((root / "client.crt").read_bytes() +
                       (root / "client.key").read_bytes() +
                       (root / "ca.crt").read_bytes())
    bundle.chmod(0o600)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(root / "server.crt", root / "server.key")
    context.load_verify_locations(root / "ca.crt")
    context.verify_mode = ssl.CERT_REQUIRED
    Handler.failure = False
    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([cli, "--lockd", f"https://127.0.0.1:{server.server_port}",
                                 "--lockd-client-pem", str(bundle), "--list"],
                                cwd=root, env=env, capture_output=True, text=True,
                                timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout == "No conversations.\n"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
