#!/usr/bin/env python3
"""Exercise terminal recovery using real prompt/renderers and failing peers."""
import http.server
import importlib.util
import json
import os
import pathlib
import re
import socketserver
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "terminal", pathlib.Path(__file__).with_name("cli_terminal_test.py"))
terminal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terminal)


class Peer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def event(self, kind, **fields):
        body = json.dumps(dict(type=kind, **fields), ensure_ascii=False)
        self.wfile.write(f"event: {kind}\ndata: {body}\n\n".encode())
        self.wfile.flush()

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(request)
        done = threading.Event()
        self.server.finished.append(done)
        release = self.server.release
        mode = self.server.mode
        label = mode.replace("_", "-")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        if mode == "truncated":
            self.send_header("Content-Length", "10000")
        self.end_headers()
        self.close_connection = True
        try:
            if mode == "malformed":
                self.wfile.write(b'event: response.output_text.delta\ndata: {bad json}\n\n')
                self.wfile.flush()
                return
            if mode != "success":
                self.event("response.reasoning_summary_text.delta",
                           delta=f"Checking {mode}")
                self.event("response.output_text.delta",
                           delta=f"PARTIAL {label} café λ.\n\n")
                if mode == "truncated":
                    return
                if mode == "provider_error":
                    self.event("error", code="fixture_failure",
                               message="fixture stream error")
                    return
                if mode in ("cancel", "escape"):
                    if not release.wait(10):
                        raise AssertionError("cancelled stream was not released")
                    self.event("response.output_text.delta", delta="STALE cancelled tail.\n\n")
            else:
                self.event("response.output_text.delta", delta="RECOVERED café λ.\n\n")
            self.event("response.completed", response={
                "id": f"resp_failure_{len(self.server.requests)}", "usage": {
                    "input_tokens": 10, "output_tokens": 10, "total_tokens": 20}})
        except (BrokenPipeError, ConnectionResetError):
            if mode not in ("cancel", "escape"):
                self.server.errors.append(f"unexpected disconnect: {mode}")
        except Exception as exc:
            self.server.errors.append(f"{mode}: {exc}")
        finally:
            done.set()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Peer)
        self.mode = "success"
        self.requests = []
        self.errors = []
        self.finished = []
        self.release = threading.Event()


class BrokenClipboard(socketserver.BaseRequestHandler):
    def handle(self):
        # Incomplete X11 setup reply followed by EOF.
        self.request.recv(4096)
        self.request.sendall(b"\x01")


def tail(term, offset):
    return re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", term.output[offset:])


cli = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="terminal-failures-", dir=cli.parent) as directory:
    root = pathlib.Path(directory)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("LOG_") and key.lower() not in (
               "http_proxy", "https_proxy", "all_proxy")}
    env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
               XDG_CACHE_HOME=str(root / "cache"),
               XDG_CONFIG_HOME=str(root / "config"),
               XDG_DATA_HOME=str(root / "data"), TERM="xterm-256color",
               CAI_API_KEY="fixture", NO_PROXY="127.0.0.1,localhost",
               no_proxy="127.0.0.1,localhost")
    server = Server()
    clipboard = socketserver.TCPServer(("127.0.0.1", 0), BrokenClipboard)
    threads = [threading.Thread(target=s.serve_forever, daemon=True)
               for s in (server, clipboard)]
    for thread in threads:
        thread.start()
    args = [str(cli), "-N", "-p", "custom", "--endpoint",
            f"http://127.0.0.1:{server.server_port}/v1", "-m", "gpt-5.6-luna",
            "--no-terminal", "--no-image-generation", "-C", str(root)]
    term = None
    try:
        for mode in ("provider_error", "truncated", "malformed", "cancel", "escape"):
            server.mode = mode
            label = mode.replace("_", "-")
            server.release = threading.Event()
            before = len(server.requests)
            term = terminal.Terminal(args, env)
            term.wait(lambda: term.contains("Welcome to CAI"))
            term.send(f"exercise {mode}\r".encode())
            if mode in ("cancel", "escape"):
                term.wait(lambda: term.contains(f"PARTIAL {label} café λ."))
                began = time.monotonic()
                term.send(b"\x03" if mode == "cancel" else b"\x1b")
            term.wait(lambda: term.contains("Worked for"))
            if mode in ("cancel", "escape"):
                assert time.monotonic() - began < 3, "cancellation waited for the provider"
            if mode not in ("cancel", "escape"):
                term.wait(lambda: term.contains("[error]"))
            if mode == "provider_error":
                term.wait(lambda: term.contains("fixture stream error"))
            if mode != "malformed":
                assert term.rendered().count(f"PARTIAL {label} café λ.".encode()) == 1
            assert len(server.requests) == before + 1, "failure retried a visible reply"
            server.release.set()
            term.wait(lambda: server.finished[before].is_set())
            # Help remains local after a failed/cancelled turn.
            term.send(b"/help\r")
            term.wait(lambda: term.contains("Run cai --help"))
            assert len(server.requests) == before + 1
            server.mode = "success"
            offset = len(term.output)
            term.send(b"continue after failure\r")
            term.wait(lambda: b"RECOVERED" in tail(term, offset) and
                      b"Worked for" in tail(term, offset))
            assert tail(term, offset).count("RECOVERED café λ.".encode()) == 1
            assert not term.contains("STALE cancelled tail"), term.rendered()
            assert len(server.requests) == before + 2
            term.close()
            term = None

        # Failed image capture must preserve the editor and leave no cache file.
        for display in (None, f"127.0.0.1:{clipboard.server_address[1] - 6000}"):
            selected_env = env.copy()
            selected_env.pop("DISPLAY", None)
            if display is not None:
                selected_env["DISPLAY"] = display
                selected_env["XAUTHORITY"] = str(root / "no-authority")
            before = len(server.requests)
            term = terminal.Terminal(args, selected_env)
            term.wait(lambda: term.contains("Welcome to CAI"))
            term.send("preserved café λ\x16\r".encode())
            term.wait(lambda: term.contains("RECOVERED") and term.contains("Worked for"))
            assert len(server.requests) == before + 1
            assert "preserved café λ" in json.dumps(server.requests[-1], ensure_ascii=False)
            assert not list((root / "cache" / "cai" / "clipboard").iterdir())
            term.close()
            term = None
        records = [json.loads(line)
                   for path in (root / "cache" / "cai").glob("*.log")
                   for line in path.read_text().splitlines()]
        assert sum(record.get("event") == "run_cancelled"
                   for record in records) == 2, "cancellation was logged as failure"
        assert any(record.get("event") == "run_failed" for record in records)
        assert not server.errors, server.errors
    finally:
        server.release.set()
        if term is not None:
            if term.process.poll() is None:
                term.process.kill()
            term.process.communicate(timeout=5)
            os.close(term.master)
        for s, thread in zip((server, clipboard), threads):
            s.shutdown()
            s.server_close()
            thread.join(timeout=5)

print("terminal stream failure, cancellation, recovery and clipboard EOF checks passed")
