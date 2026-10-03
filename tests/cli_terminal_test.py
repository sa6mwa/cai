#!/usr/bin/env python3
"""Exercise cai's real prompt, renderer and clipboard over PTY/X11 fixtures."""
import base64
import fcntl
import http.server
import json
import os
import pathlib
import pty
import re
import select
import signal
import socketserver
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time


PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8"
    "/x8AAwMCAO+aAn8AAAAASUVORK5CYII=")
JPEG = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00" + b"fixture" + b"\xff\xd9"


class Clipboard(socketserver.TCPServer):
    """Minimal X11 peer; exercises the public clipboard path, without Xorg."""

    def __init__(self):
        super().__init__(("127.0.0.1", 0), ClipboardPeer)
        self.image_type = "image/png"
        self.payload = PNG
        self.errors = []
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()

    @property
    def display(self):
        return f"127.0.0.1:{self.server_address[1] - 6000}"


class ClipboardPeer(socketserver.BaseRequestHandler):
    def read(self, length):
        result = bytearray()
        while len(result) < length:
            data = self.request.recv(length - len(result))
            if not data:
                raise EOFError
            result.extend(data)
        return bytes(result)

    def handle(self):
        try:
            self.request.settimeout(5)
            setup = self.read(12)
            assert setup[0] == ord("l")
            auth_name, auth_data = struct.unpack_from("<HH", setup, 6)
            self.read(((auth_name + 3) & ~3) + ((auth_data + 3) & ~3))
            setup_reply = bytearray(80)
            struct.pack_into("<BBHHH", setup_reply, 0, 1, 0, 11, 0, 18)
            struct.pack_into("<II", setup_reply, 12, 0x100000, 0xFFFFF)
            setup_reply[28] = 1  # one root, no pixmap formats/vendor
            struct.pack_into("<I", setup_reply, 40, 1)
            self.request.sendall(setup_reply)
            atoms = {}
            selected = 0
            sequence = 0
            while True:
                header = self.read(4)
                opcode = header[0]
                units = struct.unpack_from("<H", header, 2)[0]
                request = header + self.read(units * 4 - 4)
                sequence += 1
                reply = bytearray(32)
                struct.pack_into("<H", reply, 2, sequence)
                if opcode == 16:  # InternAtom
                    size = struct.unpack_from("<H", request, 4)[0]
                    name = request[8:8 + size].decode()
                    atom = atoms.setdefault(name, len(atoms) + 100)
                    reply[0] = 1
                    struct.pack_into("<I", reply, 8, atom)
                elif opcode == 18:  # ChangeProperty -> PropertyNotify timestamp
                    reply[0] = 28
                    reply[4:12] = request[4:12]
                    struct.pack_into("<I", reply, 12, 123)
                elif opcode == 24:  # ConvertSelection -> SelectionNotify
                    window, selection, selected, prop, timestamp = (
                        struct.unpack_from("<IIIII", request, 4))
                    available = selected == atoms.get(self.server.image_type)
                    reply[0] = 31
                    struct.pack_into("<IIIII", reply, 4, timestamp, window,
                                     selection, selected, prop if available else 0)
                elif opcode == 20:  # GetProperty, single bounded image transfer
                    payload = self.server.payload
                    reply[0:2] = b"\x01\x08"
                    struct.pack_into("<I", reply, 4, (len(payload) + 3) // 4)
                    struct.pack_into("<I", reply, 8, selected)
                    struct.pack_into("<I", reply, 16, len(payload))
                    reply.extend(payload + b"\0" * (-len(payload) % 4))
                elif opcode in (1, 19, 4):  # Create/DeleteProperty/DestroyWindow
                    continue
                else:
                    raise AssertionError(f"unexpected X11 opcode {opcode}")
                self.request.sendall(reply)
        except EOFError:
            pass
        except Exception as exc:
            self.server.errors.append(exc)


class Responses(http.server.HTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), ResponsePeer)
        self.requests = []
        self.pause = False
        self.reasoning = ["Checking the pasted input"]
        self.view_path = None
        self.plan = []
        self.pause_script = False
        self.catalog_status = 200
        self.release = threading.Event()
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()


class ResponsePeer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def event(self, kind, **fields):
        data = json.dumps(dict(type=kind, **fields))
        self.wfile.write(f"event: {kind}\ndata: {data}\n\n".encode())
        self.wfile.flush()

    def do_GET(self):
        self.send_response(self.server.catalog_status)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"data": [
            {"id": "gpt-5.6-luna", "name": "Luna"},
            {"id": "gpt-6-astra", "name": "Astra"}]}).encode())

    def do_POST(self):
        length = int(self.headers["Content-Length"])
        self.server.requests.append(json.loads(self.rfile.read(length)))
        if self.server.plan and self.server.plan[0] == "__transport_failure__":
            self.server.plan.pop(0)
            self.close_connection = True
            return
        view_path = self.server.view_path
        self.server.view_path = None
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        scripted = bool(self.server.plan)
        if scripted:
            item = self.server.plan.pop(0)
            if self.server.pause_script:
                self.server.pause_script = False
                self.event("response.output_text.delta", delta="STREAM in flight\n\n")
                assert self.server.release.wait(10), "script stream not released"
            if isinstance(item, tuple):
                name, arguments = item
                tool_item = {
                    "id": f"fc_{len(self.server.requests)}", "type": "function_call",
                    "call_id": f"call_{len(self.server.requests)}", "name": name,
                    "arguments": json.dumps(arguments)}
                if isinstance(arguments, str):
                    tool_item.pop("arguments")
                    tool_item.update(type="custom_tool_call", input=arguments)
                self.event("response.output_item.done", output_index=0, item=tool_item)
            else:
                self.event("response.output_text.delta", delta=item)
        elif view_path or not any(item.get("type") == "function_call_output"
                                for item in self.server.requests[-1].get("input", [])):
            for delta in self.server.reasoning:
                self.event("response.reasoning_summary_text.delta", delta=delta)
            self.event("response.output_text.delta", delta="EARLY paragraph.\n\n")
            if self.server.pause:
                assert self.server.release.wait(10), "stream continuation not released"
        if scripted:
            pass  # the scripted review response above is already complete
        elif view_path:
            self.event("response.output_item.done", output_index=0, item={
                "id": "fc_image", "type": "function_call", "call_id": "call_image",
                "name": "view_image", "arguments": json.dumps({"path": str(view_path)})})
        else:
            self.event("response.output_text.delta", delta="FINAL paragraph.\n\n")
        self.event("response.completed", response={
            "id": f"resp_terminal_{len(self.server.requests)}", "usage": {"input_tokens": 10,
            "output_tokens": 10, "total_tokens": 20}})


class Terminal:
    def __init__(self, args, env, width=120):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, width, 0, 0))
        self.process = subprocess.Popen(args, env=env, stdin=slave, stdout=slave,
                                        stderr=subprocess.PIPE)
        os.close(slave)
        self.output = bytearray()
        self.probe_tail = b""
        self.cursor_probes = 0

    def read(self, timeout=0.1):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return
            self.output.extend(data)
            # Count probes without answering: feeds must never wait for replies.
            probes = self.probe_tail + data
            for _ in range(probes.count(b"\x1b[6n")):
                self.cursor_probes += 1
            self.probe_tail = probes[-3:]

    def wait(self, predicate):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.read()
            if predicate():
                return
            assert self.process.poll() is None, self.output.decode(errors="replace")
        raise AssertionError(self.output.decode(errors="replace"))

    def contains(self, text):
        return text.encode() in self.rendered()

    def rendered(self):
        return re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", self.output)

    def send(self, data):
        os.write(self.master, data)

    def resize(self, width):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 30, width, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)

    def close(self):
        try:
            self.send(b"/quit\r")
            deadline = time.monotonic() + 5
            while self.process.poll() is None and time.monotonic() < deadline:
                self.read()
            assert self.process.poll() == 0, self.output.decode(errors="replace")
            self.read(0)
            errors = self.process.communicate(timeout=1)[1]
            assert not errors, errors.decode(errors="replace")
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.communicate()
            os.close(self.master)


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix="terminal-") as directory:
        root = pathlib.Path(directory)
        workspace = root / "workspace"
        workspace.mkdir()
        clipboard = Clipboard()
        api = Responses()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("LOG_") and key.lower() not in (
                   "http_proxy", "https_proxy", "all_proxy", "no_color")}
        env.update(HOME=str(root), XDG_CACHE_HOME=str(root / "cache"),
                   XDG_STATE_HOME=str(root / "state"),
                   XDG_CONFIG_HOME=str(root / "config"),
                   XDG_DATA_HOME=str(root / "data"), CAI_API_KEY="fixture",
                   DISPLAY=clipboard.display, XAUTHORITY=str(root / "no-authority"),
                   NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
        args = [str(cli), "-N", "-p", "custom", "--endpoint",
                f"http://127.0.0.1:{api.server_port}/v1", "-m", "gpt-5.6-luna",
                "--no-terminal", "--no-image-generation", "-C", str(workspace)]
        terminal = None
        try:
            for fallback, image_type, payload in (
                    (False, "image/png", PNG), (True, "image/jpeg", JPEG)):
                selected_env = env.copy()
                if fallback:
                    selected_env.pop("XDG_CACHE_HOME")
                cache = root / (".cache" if fallback else "cache")
                paste_dir = cache / "cai" / "clipboard"
                clipboard.image_type, clipboard.payload = image_type, payload
                api.pause = not fallback
                api.release.clear()
                terminal = Terminal(args, selected_env)
                terminal.wait(lambda: terminal.contains("> ") and
                              terminal.contains("Welcome to CAI. Send /help for usage."))
                assert terminal.cursor_probes <= 1, "startup repeated its cursor probe"
                assert not terminal.contains("Cai Smith. Enter sends")
                if not fallback:
                    count = len(api.requests)
                    probes = terminal.cursor_probes
                    began = time.monotonic()
                    terminal.send(b"/help\r")
                    terminal.wait(lambda: terminal.contains("Run cai --help"))
                    assert terminal.cursor_probes == probes, (
                        "help queried the terminal cursor",
                        terminal.cursor_probes - probes)
                    assert time.monotonic() - began < 1.5, "local help was delayed"
                    rendered = terminal.rendered()
                    for text in (b"CAI help", b"Slash commands", b"Prompt keys",
                                 b"/resume NUMBER", b"/export [ID]", b"Ctrl-V"):
                        assert text in rendered, rendered
                    assert b"| --- |" not in rendered, "help was not rendered as Markdown"
                    assert len(api.requests) == count, "/help started an agent turn"
                terminal.send(b"inspect this \x16")
                terminal.wait(lambda: paste_dir.exists() and bool(list(paste_dir.iterdir())))
                files = list(paste_dir.iterdir())
                assert len(files) == 1, files
                image = files[0]
                extension = "png" if image_type == "image/png" else "jpeg"
                assert re.fullmatch(r"[0-9a-v]{20}\." + extension, image.name), image
                terminal.wait(lambda: terminal.contains(str(image)))
                assert image.read_bytes() == payload
                assert stat.S_IMODE(image.stat().st_mode) == 0o600
                assert stat.S_IMODE(paste_dir.stat().st_mode) == 0o700
                assert not (cache / "softline").exists()
                api.view_path = image
                probes = terminal.cursor_probes
                terminal.send(b"\r")
                terminal.wait(lambda: terminal.contains("EARLY paragraph."))
                assert terminal.cursor_probes == probes, "live feed queried the cursor"
                if api.pause:
                    # This text must be visible before the server sends completion.
                    assert not terminal.contains("FINAL paragraph.")
                    terminal.wait(lambda: terminal.contains("Checking the pasted input"))
                    count = len(api.requests)
                    probes = terminal.cursor_probes
                    offset = len(terminal.output)
                    began = time.monotonic()
                    terminal.send(b"/help\r")
                    terminal.wait(lambda: b"Run cai --help" in re.sub(
                        rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", terminal.output[offset:]))
                    assert terminal.cursor_probes == probes, (
                        "busy help queried the terminal cursor",
                        terminal.cursor_probes - probes)
                    assert time.monotonic() - began < 1.5, "busy help was delayed"
                    assert len(api.requests) == count, "/help steered a busy agent"
                    terminal.send(b"preserved-draft")
                    terminal.resize(48)
                    terminal.wait(lambda: terminal.contains("preserved-draft"))
                    api.release.set()
                terminal.wait(lambda: terminal.contains("Worked for"))
                if api.pause:
                    terminal.resize(120)
                assert terminal.contains("FINAL paragraph.")
                rendered = terminal.rendered()
                assert rendered.count(b"EARLY paragraph.") == 1
                assert rendered.count(b"FINAL paragraph.") == 1
                assert b"! Worked for" in rendered, rendered
                assert "inspect this " + str(image) in json.dumps(api.requests[-2])
                data_url = "data:" + image_type + ";base64," + base64.b64encode(payload).decode()
                assert data_url in json.dumps(api.requests[-1]), api.requests[-1]
                if api.pause:
                    count = len(api.requests)
                    offset = len(terminal.output)
                    terminal.send(b"\r")
                    terminal.wait(lambda: len(api.requests) > count and
                                  b"Worked for" in re.sub(
                                      rb"\x1b\[[0-?]*[ -/]*[@-~]", b"",
                                      terminal.output[offset:]))
                    assert "preserved-draft" in json.dumps(api.requests[-1])
                    assert "CAI help" not in json.dumps(api.requests[-1])
                terminal.close()
                terminal = None
                assert image.exists(), "pasted file must persist after exit"

            # Resuming replays the transcript but keeps startup guidance in
            # the thinking bar. Help must not enter model history or usage.
            count = len(api.requests)
            began = time.monotonic()
            terminal = Terminal([args[0]] + args[2:], env)
            terminal.wait(lambda: terminal.contains("FINAL paragraph.") and
                          terminal.contains("Welcome to CAI. Send /help for usage."))
            assert time.monotonic() - began < 2.0, "local replay was delayed"
            assert terminal.cursor_probes <= 1, "replay added feed-time cursor probes"
            assert not terminal.contains("Cai Smith. Enter sends")
            assert len(api.requests) == count, "replay sent a model request"
            terminal.send(b"/help\r")
            terminal.wait(lambda: terminal.contains("Run cai --help"))
            assert len(api.requests) == count
            terminal.send(b"/new\r")
            terminal.wait(lambda: terminal.contains("--- session ---"))
            api.plan = ["__transport_failure__"]
            terminal.send(b"trigger a transport failure\r")
            terminal.wait(lambda: terminal.contains("HTTP transport failed") and
                          terminal.contains("Worked for"))
            rendered = terminal.rendered()
            assert b"* ERROR: HTTP transport failed: error:" in rendered, rendered
            assert b"Empty reply from server" in rendered or b"Server returned nothing" in rendered
            assert b"method: POST" in rendered and b"path: responses" in rendered, rendered
            assert f"base_url: http://127.0.0.1:{api.server_port}/v1".encode() in rendered, rendered
            assert b"cai.http.transport_error" not in rendered
            assert b"CAI help" not in json.dumps(api.requests[-1]).encode()
            terminal.close()
            terminal = None

            # Model changes preserve this conversation. A selection during
            # an in-flight request applies to the very next tool continuation.
            model_logs_before = set((root / "cache" / "cai").glob("*.log"))
            metadata = root / "models.json"
            metadata.write_text(json.dumps({"models": [
                {"provider": "custom", "id": "gpt-5.6-luna"},
                {"provider": "custom", "id": "gpt-6-astra"}]}))
            terminal = Terminal(args + ["--model-metadata", str(metadata)], env)
            terminal.wait(lambda: terminal.contains("Welcome to CAI"))
            count = len(api.requests)
            terminal.send(b"/model\r")
            terminal.wait(lambda: terminal.contains("Current:") and
                          terminal.contains("gpt-6-astra"))
            assert len(api.requests) == count
            terminal.send(b"/model gpt-6-luna\r")
            terminal.wait(lambda: terminal.contains("Model changed to gpt-6-luna"))
            api.plan = ["IDLE switch complete"]
            terminal.send(b"remember this conversation\r")
            terminal.wait(lambda: terminal.contains("IDLE switch complete") and
                          terminal.contains("Worked for"))
            assert api.requests[-1]["model"] == "gpt-6-luna"
            api.release.clear()
            api.pause_script = True
            switch_image = workspace / "switch.png"
            switch_image.write_bytes(PNG)
            api.plan = [("view_image", {"path": str(switch_image)}),
                        "SWITCH continuation"]
            terminal.send(b"inspect while switching\r")
            terminal.wait(lambda: terminal.contains("STREAM in flight"))
            count = len(api.requests)
            assert api.requests[-1]["model"] == "gpt-6-luna"
            terminal.send(b"/model gpt-5.6-luna\r")
            terminal.wait(lambda: terminal.contains("Model gpt-5.6-luna queued"))
            terminal.send(b"/model gpt-6-astra\r")
            terminal.wait(lambda: terminal.contains("Model gpt-6-astra queued"))
            terminal.send(b"/model\r")
            terminal.wait(lambda: terminal.contains("Pending:"))
            assert len(api.requests) == count
            api.release.set()
            terminal.wait(lambda: terminal.contains("SWITCH continuation"))
            assert len(api.requests) == count + 1
            assert api.requests[-1]["model"] == "gpt-6-astra"
            assert "remember this conversation" in json.dumps(api.requests[-1])
            assert any(item.get("type") == "function_call_output"
                       for item in api.requests[-1].get("input", []))
            assert "/model" not in json.dumps(api.requests[-1])
            terminal.send(b"/model one two\r")
            terminal.wait(lambda: terminal.contains("* ERROR: input: use /model MODEL"))
            assert re.search(rb"\x1b\[(?:0;)?91m", terminal.output)
            assert len(api.requests) == count + 1
            terminal.close()
            terminal = None

            model_logs = set((root / "cache" / "cai").glob("*.log")) - model_logs_before
            assert len(model_logs) == 1, model_logs
            selections = [json.loads(line) for line in next(iter(model_logs)).read_text().splitlines()
                          if json.loads(line).get("event") == "model_selection"]
            assert len(selections) == 3, selections
            assert all(record["lvl"] == "info" and record["role"] == "user"
                       for record in selections), selections
            assert [record["model"] for record in selections] == [
                "gpt-6-luna", "gpt-5.6-luna", "gpt-6-astra"]
            assert [record["applied"] for record in selections] == [True, False, False]

            # Providers without a catalog still accept direct model selection.
            terminal = Terminal(args, env)
            terminal.wait(lambda: terminal.contains("Welcome to CAI"))
            terminal.send(b"/model\r")
            terminal.wait(lambda: terminal.contains("Model list unavailable; enter a model ID."))
            terminal.send(b"/model gpt-6-luna\r")
            terminal.wait(lambda: terminal.contains("Model changed to gpt-6-luna"))
            # A steered response without tools is also a safe boundary: its
            # continuation must use the queued model rather than wait for
            # the entire steered turn to finish.
            api.release.clear()
            api.pause_script = True
            api.plan = ["OLD response completed", "STEERED response completed"]
            count = len(api.requests)
            terminal.send(b"begin a steered turn\r")
            terminal.wait(lambda: terminal.contains("STREAM in flight"))
            assert api.requests[-1]["model"] == "gpt-6-luna"
            terminal.send(b"/model gpt-6-astra\r")
            terminal.wait(lambda: terminal.contains("Model gpt-6-astra queued"))
            terminal.send(b"change course after response\r")
            terminal.wait(lambda: terminal.contains("change course after response"))
            api.release.set()
            terminal.wait(lambda: terminal.contains("STEERED response completed") and
                          terminal.contains("Worked for"))
            assert len(api.requests) == count + 2
            assert api.requests[-1]["model"] == "gpt-6-astra"
            assert "change course after response" in json.dumps(api.requests[-1])
            assert "OLD response completed" in json.dumps(api.requests[-1])
            terminal.close()
            terminal = None

            # The additional root preserves workspace access and excludes
            # siblings, prefix matches, symlink escapes and hardlinks.
            workspace_image = workspace / "workspace.png"
            workspace_image.write_bytes(PNG)
            unrelated = root / "cache" / "cai" / "other.png"
            unrelated.write_bytes(PNG)
            sibling = root / "cache" / "cai" / "clipboard-other"
            sibling.mkdir()
            (sibling / "image.png").write_bytes(PNG)
            paste_dir = root / "cache" / "cai" / "clipboard"
            escape = paste_dir / "escape.png"
            escape.symlink_to(unrelated)
            alias = paste_dir / "hardlink.png"
            os.link(workspace_image, alias)
            workspace_copy = workspace / "allowed.png"
            workspace_copy.write_bytes(PNG)
            for image, allowed in ((workspace_copy, True), (unrelated, False),
                                   (sibling / "image.png", False), (escape, False),
                                   (alias, False)):
                api.pause = False
                api.view_path = image
                result = subprocess.run(args + ["-ni", "inspect an image"], env=env,
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode == 0, result.stderr
                assert result.stdout.strip() == "FINAL paragraph.", result.stdout
                assert ("data:image/png;base64," in json.dumps(api.requests[-1])) == allowed
                if not allowed:
                    assert "error" in json.dumps(api.requests[-1]), api.requests[-1]
            escape.unlink()
            alias.unlink()

            # Both standalone review and the review subagent inherit the
            # image-only root. Their model receives typed clipboard content.
            pasted = next(paste_dir.glob("*.png"))
            report = json.dumps({"findings": [], "overall_correctness": "patch is correct",
                                 "overall_explanation": "Clipboard image checked",
                                 "overall_confidence_score": 0.9})
            for child in (False, True):
                api.plan = [("view_image", {"path": str(pasted)}), report]
                if child:
                    api.plan.insert(0, ("run_review", {"target": "custom",
                                                      "instructions": "Inspect the pasted image"}))
                    api.plan.append("FINAL paragraph.")
                count = len(api.requests)
                mode = ["-ni", "Review the pasted image"] if child else [
                    "--review", "-i", "Inspect the pasted image"]
                selected_args = args if child else [args[0]] + args[2:]
                result = subprocess.run(selected_args + mode, env=env, capture_output=True,
                                        text=True, timeout=15)
                assert result.returncode == 0, result.stderr
                assert not api.plan, api.plan
                image_request = api.requests[count + (2 if child else 1)]
                assert "data:image/png;base64," in json.dumps(image_request)
                assert ("FINAL paragraph." if child else "Clipboard image checked") in result.stdout

            # Reject a clipboard root replaced by a symlink during startup.
            original = paste_dir.with_name("clipboard-saved")
            paste_dir.rename(original)
            paste_dir.symlink_to(original, target_is_directory=True)
            result = subprocess.run(args + ["-ni", "should not run"], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode != 0 and not result.stdout
            assert "must not be a symlink" in result.stderr
            paste_dir.unlink()
            original.rename(paste_dir)

            # Unavailable targets and invalid payloads preserve the typed prompt.
            for image_type, payload in ((None, PNG), ("image/png", b"not an image")):
                clipboard.image_type, clipboard.payload = image_type, payload
                api.pause = False
                before = list((root / "cache" / "cai" / "clipboard").iterdir())
                terminal = Terminal(args, env)
                terminal.wait(lambda: terminal.contains("> "))
                terminal.send(b"unchanged-draft\x16")
                # Softline must finish the synchronous clipboard attempt before Enter.
                terminal.send(b"\r")
                terminal.wait(lambda: terminal.contains("Worked for"))
                assert "unchanged-draft" in json.dumps(api.requests[-1])
                assert list((root / "cache" / "cai" / "clipboard").iterdir()) == before
                terminal.close()
                terminal = None
            assert not clipboard.errors, clipboard.errors
        finally:
            api.release.set()
            if terminal is not None and terminal.process.poll() is None:
                terminal.process.kill()
                terminal.process.communicate()
                os.close(terminal.master)
            for server in (api, clipboard):
                server.shutdown()
                server.server_close()
                server.thread.join(timeout=5)
    print("terminal streaming, resize, status and clipboard checks passed")


if __name__ == "__main__":
    main()
