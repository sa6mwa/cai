#!/usr/bin/env python3
"""Offline Chrome extension/host fixtures. Never connects to a real browser."""
import base64
import contextlib
import http.server
import json
import os
import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
ORIGIN = "chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/"
NAME = "com.openai.codexextension.json"
PNG = ("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8"
       "/x8AAwMCAO+aAn8AAAAASUVORK5CYII=")


def wire(message):
    payload = json.dumps(message).encode()
    return struct.pack("=I", len(payload)) + payload


def exact(fd, length):
    data = bytearray()
    while len(data) < length:
        part = fd.recv(length - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)


def recv(fd):
    return json.loads(exact(fd, struct.unpack("=I", exact(fd, 4))[0]))


def reply(request, result):
    return {"jsonrpc": "2.0", "id": request["id"], "result": result}


def identity():
    return {"type": "extension", "family": "chrome", "name": "Chrome"}


def private_env(root):
    env = os.environ.copy()
    # Never inherit a production host's paths, settings or credentials.
    for key in list(env):
        if key.startswith("LOG_") or key in ("CHROME_CONFIG_HOME", "CODEX_HOME"):
            env.pop(key)
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
               XDG_CACHE_HOME=str(root / "cache"), XDG_RUNTIME_DIR=str(root / "run"),
               XDG_STATE_HOME=str(root / "state"), XDG_DATA_HOME=str(root / "data"),
               CAI_API_KEY="fixture", LOG_LEVEL="warn", LOG_MODE="json",
               NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
    (root / "run").mkdir(mode=0o700)
    return env


@contextlib.contextmanager
def mock_socket(root, handler, name="mock.sock"):
    path = root / name
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(path))
    path.chmod(0o600)
    listener.listen(8)
    listener.settimeout(3)
    errors = []

    def serve():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                handler(connection)
        except (EOFError, BrokenPipeError, ConnectionResetError):
            pass
        except BaseException as error:
            errors.append(error)

    thread = threading.Thread(target=serve)
    thread.start()
    try:
        yield path
    finally:
        thread.join(4)
        listener.close()
        assert not thread.is_alive()
        assert not errors, errors
        path.unlink(missing_ok=True)


def assert_equal(actual, expected):
    assert actual == expected, (actual, expected)


def client_tests(binary, root):
    def run(mode, handler, expect=0, needle=None, args=()):
        with mock_socket(root, handler) as path:
            result = subprocess.run([binary, str(path), mode, *args], capture_output=True, timeout=5)
        assert result.returncode == expect, (mode, result.stdout, result.stderr)
        assert b"AddressSanitizer:" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr
        if expect:
            assert b"status=" in result.stderr, (mode, result.stderr)
        if needle:
            assert needle in result.stdout + result.stderr, (mode, result.stdout, result.stderr)
        return result

    def handshake(fd):
        message = recv(fd)
        assert message["method"] == "getInfo"
        assert set(message) == {"jsonrpc", "id", "method", "params"}
        fd.sendall(wire(reply(message, identity())))

    def success(fd):
        handshake(fd)
        message = recv(fd)
        fd.sendall(wire({"jsonrpc": "2.0", "method": "onCDPEvent", "params": {}}))
        for byte in wire(reply(message, [{"id": 7, "title": "Fixture"}])):
            fd.sendall(bytes([byte]))
    run("request", success, needle=b"Fixture")

    def null(fd):
        handshake(fd)
        fd.sendall(wire(reply(recv(fd), None)))
    run("request", null, needle=b"null")

    def fail(fd):
        handshake(fd)
        message = recv(fd)
        fd.sendall(wire({"jsonrpc": "2.0", "id": message["id"],
                         "error": {"code": -32000, "message": "Debugger unattached"}}))
    run("request", fail, 1, b"Debugger unattached")
    run("request", lambda fd: (handshake(fd), recv(fd), fd.sendall(wire({"jsonrpc": "2.0", "id": "wrong", "result": {}}))), 1, b"request ID")
    run("request", lambda fd: (handshake(fd), recv(fd), fd.sendall(struct.pack("=I", 0xffffffff))), 1, b"limit")
    run("request", lambda fd: (handshake(fd), recv(fd), fd.sendall(struct.pack("=I", 2) + b"{{")), 1, b"Invalid Chrome")
    run("request", lambda fd: (handshake(fd), recv(fd), fd.sendall(struct.pack("=I", 10) + b"short")), 1, b"disconnected")
    run("request", lambda fd: (handshake(fd), time.sleep(.6)), 1, b"timed out")
    run("cancel", lambda fd: (handshake(fd), time.sleep(.3)), 1, b"cancelled")
    run("open", lambda fd: fd.sendall(wire(reply(recv(fd), {"type": "iab", "family": "chrome"}))), 1, b"not a Chrome")
    for info in ({"type": None, "family": "chrome"}, {"type": "extension", "family": None}, None):
        run("open", lambda fd: fd.sendall(wire(reply(recv(fd), info))), 1)
    run("request", lambda fd: (handshake(fd), fd.sendall(wire({"jsonrpc": "2.0", "id": recv(fd)["id"]}))), 1, b"result")
    run("limit", lambda fd: (handshake(fd), fd.sendall(wire(reply(recv(fd), "x" * 300)))), 1, b"limit")

    calls = []
    def tools(fd):
        handshake(fd)
        while True:
            message = recv(fd)
            calls.append(message)
            assert message["params"]["session_id"] == "fixture"
            assert message["params"]["turn_id"] == "1"
            fd.sendall(wire(reply(message, {"id": 7} if message["method"] == "createTab" else {})))
    result = run("tool", tools, args=(json.dumps({"action": "open", "url": "https://example.test/"}),))
    assert b'"status":"ok"' in result.stdout
    assert [m["method"] for m in calls] == ["createTab", "attach", "executeCdp", "turnEnded"]
    assert calls[2]["params"]["target"] == {"tabId": 7}
    assert calls[2]["params"]["method"] == "Page.navigate"
    assert calls[2]["params"]["params"]["url"] == "https://example.test/"
    assert calls[3]["params"]["session_context"] == "cached"
    for args in ({"action": "click", "tab_id": 1}, {"action": "navigate", "tab_id": 1, "url": "file:///etc/passwd"}, {"action": "open", "url": "file:///etc/passwd"}, {"action": "nonsense", "tab_id": 1}):
        run("tool", lambda fd: (handshake(fd), recv(fd)), 1, args=(json.dumps(args),))
    for args in ({"action": None}, {"action": ""}, {}, [], None, {"action": "key", "tab_id": 7, "text": None}, {"action": "cdp", "tab_id": 7, "method": "Runtime.evaluate", "params": None}):
        run("tool", lambda fd: (handshake(fd), recv(fd)), 1, args=(json.dumps(args),))

    # Every built-in interaction emits the expected CDP operation, and each
    # used turn is released even after a tool/provider error.
    def exercise(arguments, expected_methods, check=None, image=None, expect=0):
        seen = []
        def peer(fd):
            handshake(fd)
            while True:
                message = recv(fd)
                seen.append(message)
                value = {"id": 7} if message["method"] == "claimUserTab" else {"data": image} if image is not None and message["method"] == "executeCdp" else None
                fd.sendall(wire(reply(message, value)))
        result = run("tool", peer, expect, args=(json.dumps(arguments),))
        assert [m["method"] for m in seen] == expected_methods + ["turnEnded"], seen
        if check:
            check(seen)
        return result
    exercise({"action": "claim", "tab_id": 7}, ["claimUserTab", "attach"])
    exercise({"action": "tabs"}, ["getUserTabs"])
    exercise({"action": "snapshot", "tab_id": 7}, ["executeCdp"],
             lambda s: assert_equal(s[0]["params"]["params"]["returnByValue"], True))
    exercise({"action": "click", "tab_id": 7, "x": 12, "y": 20}, ["executeCdp", "executeCdp"],
             lambda s: assert_equal([m["params"]["params"]["type"] for m in s[:2]], ["mousePressed", "mouseReleased"]))
    exercise({"action": "key", "tab_id": 7, "text": "Enter"}, ["executeCdp", "executeCdp"],
             lambda s: assert_equal(s[0]["params"]["params"]["windowsVirtualKeyCode"], 13))
    exercise({"action": "scroll", "tab_id": 7, "x": 1, "y": 2, "delta_y": -10}, ["executeCdp"],
             lambda s: assert_equal(s[0]["params"]["params"]["deltaX"], 0))
    for action, extra in (("type", {"text": 'quote"\nline'}), ("evaluate", {"text": "document.title"}), ("keep", {}), ("close", {}), ("navigate", {"url": "https://example.test/?a=b"}), ("cdp", {"method": "Runtime.evaluate", "params": {"expression": 'JSON.stringify("quoted")', "returnByValue": True}})):
        exercise(dict(action=action, tab_id=7, **extra), ["markTab" if action == "keep" else "executeCdp"])
    result = exercise({"action": "screenshot", "tab_id": 7}, ["executeCdp"], image=PNG)
    assert PNG.encode() not in result.stdout and b"attached" in result.stdout
    exercise({"action": "screenshot", "tab_id": 7}, ["executeCdp"], image="invalid", expect=1)
    def null_image(fd):
        handshake(fd)
        message = recv(fd)
        fd.sendall(wire(reply(message, {"data": None})))
        message = recv(fd)
        assert message["method"] == "turnEnded"
        fd.sendall(wire(reply(message, None)))
    run("tool", null_image, 1, b"no image data", args=(json.dumps({"action": "screenshot", "tab_id": 7}),))
    large_image = base64.b64encode(base64.b64decode(PNG) + bytes(200000)).decode()
    result = exercise({"action": "screenshot", "tab_id": 7}, ["executeCdp"], image=large_image)
    assert large_image.encode() not in result.stdout
    run("request", lambda fd: (handshake(fd), fd.sendall(wire(reply(recv(fd), "x" * 200000)))), needle=b"xxx")
    run("request", lambda fd: (handshake(fd), fd.sendall(wire({"jsonrpc": "2.0", "id": recv(fd)["id"], "error": {"code": -1}}))), 1, b"complete error")
    run("request", lambda fd: (handshake(fd), fd.sendall(wire({"jsonrpc": "2.0", "id": recv(fd)["id"], "result": None, "error": {"code": -1, "message": "mixed"}}))), 1, b"both result and error")
    run("request", lambda fd: (handshake(fd), recv(fd), fd.sendall(wire({"jsonrpc": "2.0", "method": "notification", "result": None}))), 1, b"invalid reply members")
    def duplicate(fd):
        handshake(fd)
        message = recv(fd)
        data = ('{"jsonrpc":"2.0","id":' + json.dumps(message["id"]) + ',"result":1,"result":2}').encode()
        fd.sendall(struct.pack("=I", len(data)) + data)
    run("request", duplicate, 1, b"duplicate")

    # No sockets and insecure paths fail locally, without a browser probe.
    empty = root / "empty"
    empty.mkdir(mode=0o700)
    result = subprocess.run([binary, str(empty), "discover"], capture_output=True)
    assert result.returncode and b"No cai Chrome host" in result.stderr
    empty.chmod(0o755)
    result = subprocess.run([binary, str(empty), "discover"], capture_output=True)
    assert result.returncode and b"private" in result.stderr
    print("Chrome client: framing, identity, errors, cancellation, limits, tool calls and cleanup passed")


class AgentAPI(http.server.HTTPServer):
    def __init__(self, mode):
        super().__init__(("127.0.0.1", 0), AgentPeer)
        self.mode = mode
        self.requests = []
        self.plan = [{"action": "open", "url": "https://example.test/"},
                     {"action": "screenshot", "tab_id": 7},
                     {"action": "keep", "tab_id": 7}, "Chrome final response"] if mode == "runtime-chat" else [{"action": "open"}, "unused final"]


class AgentPeer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        payload = json.dumps({"data": [{"id": "gpt-5.6-luna", "context_length": 128000}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(body)
        count = len(self.server.requests)
        if self.server.mode == "runtime-failure" and count > 1:
            payload = json.dumps({"error": {"message": "Fixture provider failure", "code": "fixture_failure"}}).encode()
            self.send_response(400)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if self.server.mode == "runtime-cancel" and count > 1:
            # Wait for actual cancellation, rather than racing a final model
            # response against libcurl's periodic cancellation callback.
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.flush()
            self.connection.settimeout(4)
            assert self.connection.recv(1) == b"", "runtime did not cancel the pending response"
            return
        item = self.server.plan.pop(0)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        try:
            if self.path.endswith("/chat/completions"):
                delta = {"content": item} if isinstance(item, str) else {"tool_calls": [{"index": 0, "id": f"call_{count}", "type": "function", "function": {"name": "chrome", "arguments": json.dumps(item)}}]}
                events = [{"id": f"chat_{count}", "choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                          {"id": f"chat_{count}", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop" if isinstance(item, str) else "tool_calls"}]},
                          {"id": f"chat_{count}", "choices": [], "usage": {"prompt_tokens": 10, "completion_tokens": 10, "total_tokens": 20}}]
                for event in events:
                    self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
                self.wfile.write(b"data: [DONE]\n\n")
            else:
                event = {"type": "response.output_text.delta", "delta": item} if isinstance(item, str) else {"type": "response.output_item.done", "output_index": 0, "item": {"type": "function_call", "name": "chrome", "arguments": json.dumps(item), "call_id": f"call_{count}", "id": f"fc_{count}"}}
                completed = {"type": "response.completed", "response": {"id": f"resp_{count}", "usage": {"input_tokens": 10, "output_tokens": 10, "total_tokens": 20}}}
                for value in (event, completed):
                    self.wfile.write(("event: " + value["type"] + "\ndata: " + json.dumps(value) + "\n\n").encode())
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def runtime_tests(binary, root):
    for mode in ("runtime-chat", "runtime-failure", "runtime-cancel", "runtime-cleanup-failure"):
        api = AgentAPI(mode)
        thread = threading.Thread(target=api.serve_forever, daemon=True)
        thread.start()
        seen = []
        def peer(fd):
            message = recv(fd)
            fd.sendall(wire(reply(message, identity())))
            while True:
                message = recv(fd)
                seen.append(message)
                method, params = message["method"], message["params"]
                value = {"id": 7} if method == "createTab" else {"data": PNG} if method == "executeCdp" and params["method"] == "Page.captureScreenshot" else None
                if mode == "runtime-cleanup-failure" and method == "turnEnded":
                    fd.sendall(wire({"jsonrpc": "2.0", "id": message["id"], "error": {"code": -32000, "message": "Fixture cleanup rejected"}}))
                    assert fd.recv(1) == b"", "cleanup failure must invalidate the connection"
                    return
                fd.sendall(wire(reply(message, value)))
        try:
            with mock_socket(root, peer) as path:
                result = subprocess.run([binary, str(path), mode, f"http://127.0.0.1:{api.server_port}/v1", str(root)], capture_output=True, timeout=10)
            assert result.returncode == (0 if mode == "runtime-chat" else 1), (mode, result.stdout, result.stderr, seen)
            assert seen[-1]["method"] == "turnEnded", (mode, seen)
            assert len({m["params"]["session_id"] for m in seen}) == 1
            if mode == "runtime-chat":
                assert result.stdout == b"Chrome final response"
                assert len(api.requests) == 4
                image_message = json.dumps(api.requests[2]["messages"])
                assert '"type": "image_url"' in image_message and "data:image/png;base64," + PNG in image_message
                assert PNG not in json.dumps(api.requests[3]), "Chat screenshot leaked into durable history"
            elif mode == "runtime-cleanup-failure":
                assert result.stdout == b"unused final", (mode, result.stdout)
            else:
                assert result.stdout == b"", (mode, result.stdout)
        finally:
            api.shutdown()
            api.server_close()
    print("Chrome runtime: Chat Completions image inference plus provider failure/cancellation cleanup passed")


def lua_tests(binary, root):
    for finish in ("close", "gc"):
        api = AgentAPI("runtime-chat")
        threading.Thread(target=api.serve_forever, daemon=True).start()
        seen = []
        def peer(fd):
            message = recv(fd)
            fd.sendall(wire(reply(message, identity())))
            while True:
                message = recv(fd)
                seen.append(message)
                method, params = message["method"], message["params"]
                value = {"id": 7} if method == "createTab" else {"data": PNG} if method == "executeCdp" and params["method"] == "Page.captureScreenshot" else None
                fd.sendall(wire(reply(message, value)))
        try:
            script = pathlib.Path(__file__).parent / "lua/e2e_chrome.lua"
            home = root / finish
            home.mkdir()
            env = private_env(home)
            with mock_socket(root, peer) as path:
                result = subprocess.run([binary, str(script), str(path), f"http://127.0.0.1:{api.server_port}/v1", str(home), finish], env=env, capture_output=True, timeout=10)
            assert result.returncode == 0, (finish, result.stdout, result.stderr)
            assert seen[-1]["method"] == "turnEnded"
            assert len(api.requests) == 4
            assert PNG in json.dumps(api.requests[2]) and PNG not in json.dumps(api.requests[3])
        finally:
            api.shutdown()
            api.server_close()
    print("Lua Chrome runtime: tool execution, screenshot delivery, explicit close and garbage collection passed")


class Host:
    def __init__(self, binary, env):
        # Exact argv used by Chrome: it appends the extension origin to path.
        self.process = subprocess.Popen([binary, ORIGIN], env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.env = env
        self.directory = pathlib.Path(env["XDG_RUNTIME_DIR"]) / "cai/chrome"
        self.path = self.directory / f"{self.process.pid}.sock"
        self.buffer = bytearray()
        deadline = time.monotonic() + 3
        while not self.path.exists() and time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(self.process.communicate())
            time.sleep(.01)
        assert self.path.exists()
        assert self.path.stat().st_mode & 0o777 == 0o700
        assert self.directory.stat().st_mode & 0o777 == 0o700

    def send(self, message, fragment=False):
        data = wire(message)
        if fragment:
            for byte in data:
                self.process.stdin.write(bytes([byte]))
                self.process.stdin.flush()
        else:
            self.process.stdin.write(data)
            self.process.stdin.flush()

    def read(self, timeout=3):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 4:
                length = struct.unpack("=I", self.buffer[:4])[0]
                if len(self.buffer) >= length + 4:
                    message = json.loads(self.buffer[4:4 + length])
                    del self.buffer[:4 + length]
                    return message
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([self.process.stdout], [], [], remaining)[0], ("host timed out", self.buffer)
            data = os.read(self.process.stdout.fileno(), 65536)
            assert data, self.process.stderr.read().decode()
            self.buffer.extend(data)

    def close(self, expect=0):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.stdin = None
        stdout, stderr = self.process.communicate(timeout=5)
        assert self.process.returncode == expect, (stdout, stderr)
        assert not self.path.exists(), "host left its socket behind"
        return stderr


def registration_tests(binary, root, env, fixture):
    def command(*args):
        return subprocess.run([binary, *args], env=env, capture_output=True, timeout=5)
    manifest = root / "config/google-chrome/NativeMessagingHosts" / NAME
    assert command("--chrome-remove").returncode == 0
    result = command("--chrome-setup")
    assert result.returncode == 0, result.stderr
    doc = json.loads(manifest.read_text())
    assert doc["path"] == binary and doc["name"] == "com.openai.codexextension"
    assert doc["allowed_origins"] == [ORIGIN] and doc["type"] == "stdio"
    assert manifest.stat().st_mode & 0o777 == 0o600
    assert not (root / "state").exists(), "utility unexpectedly opened session storage"
    content = manifest.read_bytes()
    assert command("--chrome-setup").returncode == 0
    assert manifest.read_bytes() == content
    assert command("--chrome-remove").returncode == 0 and not manifest.exists()
    assert command("--chrome-remove").returncode == 0
    malformed = [dict(doc, **{field: None}) for field in ("name", "path", "type", "description")]
    malformed += [dict(doc, allowed_origins=[None])]
    for payload in (b"not-json", json.dumps(dict(doc, path="/opt/openai/extension-host")).encode(), json.dumps(dict(doc, description="other")).encode(), *(json.dumps(value).encode() for value in malformed)):
        manifest.write_bytes(payload)
        manifest.chmod(0o600)
        for flag in ("--chrome-setup", "--chrome-remove"):
            result = command(flag)
            assert result.returncode and b"unchanged" not in result.stdout
            assert b"registered" in result.stderr and manifest.read_bytes() == payload, (result.returncode, result.stdout, result.stderr)
        manifest.unlink()
    target = root / "foreign.json"
    target.write_text("do not touch")
    manifest.symlink_to(target)
    assert command("--chrome-setup").returncode
    assert command("--chrome-remove").returncode
    assert target.read_text() == "do not touch" and manifest.is_symlink()
    manifest.unlink()
    for payload, mode in ((b"x" * 20000, 0o600), (content, 0o666)):
        manifest.write_bytes(payload)
        manifest.chmod(mode)
        result = command("--chrome-setup")
        assert result.returncode and b"unsafe" in result.stderr
        assert manifest.read_bytes() == payload
        manifest.unlink()
    os.mkfifo(manifest, 0o600)
    assert command("--chrome-setup").returncode, "non-regular manifest must not block or be replaced"
    manifest.unlink()
    manifest.parent.chmod(0o770)
    result = command("--chrome-setup")
    assert result.returncode and b"writable by others" in result.stderr
    assert not manifest.exists()
    manifest.parent.chmod(0o700)
    # Concurrent CAI installers may succeed idempotently or detect a race;
    # the visible file must always be a complete valid manifest.
    installers = [subprocess.Popen([binary, "--chrome-setup"], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
    for installer in installers:
        installer.communicate(timeout=5)
        assert installer.returncode in (0, 1)
    assert json.loads(manifest.read_text()) == doc
    assert not list(manifest.parent.glob(".cai-chrome-*.tmp"))
    assert command("--chrome-remove").returncode == 0
    # Explicit alternate browser location and argument validation.
    alternate = root / "chromium/NativeMessagingHosts" / NAME
    assert command("--chrome-setup", "--chrome-manifest", str(alternate)).returncode == 0
    assert alternate.exists() and not manifest.exists()
    assert command("--chrome-remove", "--chrome-manifest", str(alternate)).returncode == 0
    for args in (("--chrome-setup", "--chrome-remove"), ("--chrome-setup", "-ni", "work"), ("--chrome-manifest", str(manifest)), ("--review", "--chrome"), ("--chrome-setup", "--list")):
        assert command(*args).returncode == 2, args
    result = command("--chrome-native-host", "chrome-extension://wrong/")
    assert result.returncode and b"official ChatGPT" in result.stderr
    # Exercise the same system-registration guard with a private path, without
    # creating or replacing anything under /etc on the test machine.
    system = root / "system" / NAME
    system.parent.mkdir()
    def guard():
        return subprocess.run([fixture, "--test-chrome-system", str(system)], env=env, capture_output=True, timeout=5)
    assert guard().returncode == 0
    system.write_text("foreign system registration")
    result = guard()
    assert result.returncode and b"will not shadow" in result.stderr
    assert system.read_text() == "foreign system registration"
    system.unlink()
    system.symlink_to(root / "missing-host.json")
    assert guard().returncode and system.is_symlink()
    system.unlink()
    print("Chrome registration: idempotence, conflicts, symlinks, removal and CLI validation passed")


def host_tests(binary, env, discovery=True):
    host = Host(binary, dict(env, LOG_OUTPUT="stdout", LOG_LEVEL="debug"))
    host.send({"jsonrpc": "2.0", "id": 100, "method": "ping"}, fragment=True)
    assert host.read() == {"jsonrpc": "2.0", "id": 100, "result": "pong"}
    host.send({"jsonrpc": "2.0", "id": "native-host:1", "method": "codexRuntime/hello"})
    assert host.read()["error"]["code"] == -32601
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3)
        client.connect(str(host.path))
        request = {"jsonrpc": "2.0", "id": "test:1", "method": "getInfo", "params": {}}
        client.sendall(wire(request))
        assert host.read() == request
        host.send(reply(request, identity()), fragment=True)
        assert recv(client) == reply(request, identity())
        with socket.socket(socket.AF_UNIX) as busy:
            busy.settimeout(3)
            busy.connect(str(host.path))
            assert busy.recv(1) == b"", "host must reject a second owner"
        # A partially delivered client frame cannot block an idle heartbeat.
        data = wire(dict(request, id="test:2"))
        client.sendall(data[:2])
        host.send({"jsonrpc": "2.0", "id": 101, "method": "ping"})
        assert host.read()["result"] == "pong"
        client.sendall(data[2:])
        request = host.read()
        host.send(reply(request, identity()))
        assert recv(client)["id"] == "test:2"
        notification = {"jsonrpc": "2.0", "method": "onCDPDetach", "params": {"tabId": 7}}
        host.send(notification)
        assert recv(client) == notification
        request = {"jsonrpc": "2.0", "id": "test:3", "method": "claimUserTab",
                   "params": {"tabId": 7, "session_id": "s", "turn_id": "t", "session_context": "live"}}
        client.sendall(wire(request))
        assert host.read() == request
        # Disconnect during an operation: cleanup waits for its response.
    host.send(reply(request, {"id": 7}))
    cleanup = host.read()
    assert cleanup["method"] == "turnEnded" and cleanup["params"]["session_id"] == "s"
    host.send(reply(cleanup, None))
    host.send({"jsonrpc": "2.0", "id": 102, "method": "ping"})
    assert host.read()["result"] == "pong"
    host.close()
    # Malformed client input drops only that client; the extension transport
    # continues servicing heartbeats and subsequent connections.
    host = Host(binary, env)
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3)
        client.connect(str(host.path))
        client.sendall(struct.pack("=I", 0xffffffff))
        assert client.recv(1) == b""
    host.send({"jsonrpc": "2.0", "id": "still-alive", "method": "ping"})
    assert host.read()["result"] == "pong"
    # Switching a live browser lease's identity is rejected and the original
    # identity, not the attempted replacement, is released on disconnect.
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3)
        client.connect(str(host.path))
        request = {"jsonrpc": "2.0", "id": "claim", "method": "claimUserTab",
                   "params": {"tabId": 7, "session_id": "original", "turn_id": "1", "session_context": "live"}}
        client.sendall(wire(request))
        assert host.read() == request
        host.send(reply(request, {"id": 7}))
        assert recv(client)["id"] == "claim"
        client.sendall(wire(dict(request, id="changed", params=dict(request["params"], session_id="replacement"))))
        assert client.recv(1) == b""
    cleanup = host.read()
    assert cleanup["method"] == "turnEnded" and cleanup["params"]["session_id"] == "original"
    host.send(reply(cleanup, None))
    host.send({"jsonrpc": "2.0", "id": "released", "method": "ping"})
    assert host.read()["result"] == "pong"
    host.close()
    # Malformed native messaging is fatal and removes its endpoint.
    host = Host(binary, env)
    host.process.stdin.write(struct.pack("=I", 0xffffffff))
    host.process.stdin.flush()
    stderr = host.close(1)
    assert b"oversized" in stderr
    host = Host(binary, env)
    host.process.stdin.write(struct.pack("=I", 4) + b"{{{{")
    host.process.stdin.flush()
    assert b"Invalid Chrome" in host.close(1)
    # A syntactically valid envelope without a result is still an invalid
    # response and must not be treated as successful cleanup/identity.
    host = Host(binary, env)
    with socket.socket(socket.AF_UNIX) as client:
        client.connect(str(host.path))
        client.sendall(wire({"jsonrpc": "2.0", "id": "incomplete", "method": "getInfo", "params": {}}))
        request = host.read()
        host.send({"jsonrpc": "2.0", "id": request["id"]})
        assert b"complete error" in host.close(1)
    if discovery:
        host = Host(binary, env)
        second = Host(binary, env)
        try:
            result = subprocess.run([binary, "--chrome", "-Nni", "fixture", "-p", "custom", "--endpoint", "http://127.0.0.1:1/v1", "-m", "fixture"], env=env, capture_output=True, timeout=5)
            assert result.returncode and b"Multiple Chrome hosts" in result.stderr
        finally:
            second.close()
            host.close()
    print("Chrome native host: idle/fragmented heartbeats, routing, notifications, disconnect cleanup and invalid frames passed")


def agent_tests(binary, root, env):
    # Real cai runtime + Responses transport + our own native host, with only
    # the extension/browser endpoint replaced by an offline peer.
    import cli_terminal_test as terminal_fixture
    api = terminal_fixture.Responses()
    api.pause = False
    api.plan = [("chrome", {"action": "open", "url": "https://example.test/"}),
                ("chrome", {"action": "screenshot", "tab_id": 7}),
                ("chrome", {"action": "keep", "tab_id": 7}), "Chrome final response"]
    server = threading.Thread(target=api.serve_forever, daemon=True)
    server.start()
    host = Host(binary, env)
    calls, errors = [], []

    def extension():
        try:
            while True:
                request = host.read(10)
                calls.append(request)
                assert request["jsonrpc"] == "2.0"
                method, params = request["method"], request.get("params", {})
                if method == "getInfo":
                    value = identity()
                elif method == "createTab":
                    value = {"id": 7, "title": "Fixture"}
                elif method == "executeCdp" and params["method"] == "Page.captureScreenshot":
                    value = {"data": PNG}
                else:
                    value = None
                host.send(reply(request, value))
                if method == "turnEnded":
                    return
        except BaseException as error:
            errors.append(error)
    extension_thread = threading.Thread(target=extension)
    extension_thread.start()
    workspace = root / "workspace"
    workspace.mkdir()
    args = [binary, "-Nni", "Inspect the browser", "-p", "custom", "--endpoint",
            f"http://127.0.0.1:{api.server_port}/v1", "-m", "gpt-5.6-luna", "--chrome",
            "--no-terminal", "--no-image-generation", "-C", str(workspace)]
    try:
        result = subprocess.run(args, env=dict(env, LOG_LEVEL="trace"), capture_output=True, timeout=20)
        extension_thread.join(3)
        assert not extension_thread.is_alive() and not errors, errors
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr[-3500:], calls, len(api.requests))
        assert result.stdout.strip() == b"Chrome final response", result.stdout
        assert any(tool.get("name") == "chrome" for tool in api.requests[0]["tools"])
        assert [call["method"] for call in calls] == ["getInfo", "createTab", "attach", "executeCdp", "executeCdp", "markTab", "turnEnded"]
        assert calls[3]["params"]["target"] == {"tabId": 7}
        image_request = api.requests[2]
        outputs = [i for i in image_request["input"] if i.get("type") == "function_call_output"]
        assert any(isinstance(o.get("output"), list) and any(p.get("type") == "input_image" and p.get("image_url") == "data:image/png;base64," + PNG for p in o["output"]) for o in outputs), image_request
        assert PNG not in json.dumps(api.requests[3]), "typed image leaked into durable history"
        assert len({call["params"]["session_id"] for call in calls[1:]}) == 1
        assert calls[-1]["params"]["session_context"] == "cached"
    finally:
        api.shutdown()
        api.server_close()
        host.close()
    print("Chrome agent: native host, navigation, typed image inference, keep, cleanup and final stdout passed")


def interactive_tests(binary, root, env):
    import cli_terminal_test as terminal_fixture
    api = terminal_fixture.Responses()
    api.pause = False
    api.plan = [("chrome", {"action": "tabs"}), "First browser turn",
                ("chrome", {"action": "tabs"}), "Second browser turn"]
    threading.Thread(target=api.serve_forever, daemon=True).start()
    host = Host(binary, env)
    calls, errors = [], []
    def extension():
        try:
            ended = 0
            while ended < 2:
                request = host.read(timeout=15)
                calls.append(request)
                host.send(reply(request, identity() if request["method"] == "getInfo" else [] if request["method"] == "getUserTabs" else None))
                ended += request["method"] == "turnEnded"
        except BaseException as error:
            errors.append(error)
    thread = threading.Thread(target=extension)
    thread.start()
    args = [binary, "-Ni", "First Chrome turn", "-p", "custom", "--endpoint",
            f"http://127.0.0.1:{api.server_port}/v1", "-m", "gpt-5.6-luna", "--chrome",
            "--no-terminal", "--no-image-generation", "-C", str(root / "workspace")]
    terminal = terminal_fixture.Terminal(args, dict(env, TERM="xterm-256color"))
    try:
        terminal.wait(lambda: terminal.contains("First browser turn") and terminal.contains("Worked for"))
        assert terminal.contains("* Used Chrome tabs"), terminal.rendered()
        terminal.send(b"/new\r")
        terminal.wait(lambda: sum(c["method"] == "getInfo" for c in calls) == 2)
        terminal.send(b"Second Chrome turn\r")
        terminal.wait(lambda: terminal.contains("Second browser turn") and sum(c["method"] == "turnEnded" for c in calls) == 2)
        thread.join(3)
        assert not thread.is_alive() and not errors, errors
        identities = [c["params"]["session_id"] for c in calls if c["method"] == "getUserTabs"]
        assert len(set(identities)) == 2, identities
        terminal.close()
    finally:
        if terminal.process.poll() is None:
            terminal.process.kill()
            terminal.process.communicate()
            os.close(terminal.master)
        api.shutdown()
        api.server_close()
        host.close()
    print("Chrome interactive: readable receipts, session change and fresh connection passed")


def main():
    mode, binary, build = sys.argv[1:4]
    binary = str(pathlib.Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix="chrome-", dir=build) as directory:
        root = pathlib.Path(directory)
        root.chmod(0o700)
        if mode == "client":
            client_tests(binary, root)
            runtime_tests(binary, root)
        elif mode == "lua":
            lua_tests(binary, root)
        elif mode == "host":
            host_tests(binary, private_env(root), discovery=False)
        else:
            env = private_env(root)
            registration_tests(binary, root, env, sys.argv[4])
            host_tests(binary, env)
            agent_tests(binary, root, env)
            interactive_tests(binary, root, env)


if __name__ == "__main__":
    main()
