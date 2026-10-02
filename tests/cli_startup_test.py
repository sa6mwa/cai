#!/usr/bin/env python3
"""Prove a blocked quota transport cannot delay the production prompt."""
import base64
import fcntl
import importlib.util
import json
import os
import pathlib
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "terminal", pathlib.Path(__file__).with_name("cli_terminal_test.py"))
terminal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terminal)


class Terminal(terminal.Terminal):
    def __init__(self, args, env, inherited):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 30, 120, 0, 0))
        self.process = subprocess.Popen(args, env=env, stdin=slave,
                                        stdout=slave, stderr=subprocess.PIPE,
                                        pass_fds=inherited)
        os.close(slave)
        self.cursor_probes = 0
        self.output = bytearray()
        self.probe_tail = b""


def started(fd):
    term.wait(lambda: bool(select.select([fd], [], [], 0)[0]))
    assert os.read(fd, 1) == b"Q"


cli = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="startup-", dir=cli.parent) as directory:
    root = pathlib.Path(directory)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("LOG_")}
    env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
               XDG_CACHE_HOME=str(root / "cache"),
               XDG_CONFIG_HOME=str(root / "config"),
               XDG_DATA_HOME=str(root / "data"), TERM="xterm-256color")
    codex = root / ".codex"
    codex.mkdir()
    payload = base64.urlsafe_b64encode(json.dumps({"exp": 4102444800}).encode()).rstrip(b"=").decode()
    (codex / "auth.json").write_text(json.dumps({"auth_mode": "chatgpt", "tokens": {
        "id_token": "fixture", "access_token": f"e30.{payload}.fixture",
        "refresh_token": "fixture", "account_id": "fixture"}}))
    began_read, began_write = os.pipe()
    release_read, release_write = os.pipe()
    env.update(CAI_TEST_QUOTA_STARTED=str(began_write),
               CAI_TEST_QUOTA_RELEASE=str(release_read))
    term = Terminal([str(cli), "-N", "-C", str(root)], env,
                    (began_write, release_read))
    try:
        started(began_read)
        # Transport is still held open when the welcome and prompt appear.
        term.wait(lambda: term.contains("Welcome to CAI"))
        term.send(b"/help\r")
        term.wait(lambda: term.contains("/resume"))
        assert not select.select([began_read], [], [], 0)[0]
        os.write(release_write, b"1")
        term.wait(lambda: term.contains("w 88%") and term.contains("crd 20"))
        # No polling occurs while the application is idle.
        assert not select.select([began_read], [], [], 0.1)[0]
        # /status always fetches anew and updates both metrics and prompt bar.
        term.send(b"/status\r")
        started(began_read)
        os.write(release_write, b"E")
        term.wait(lambda: term.contains("Status"))
        # A failed request still consumes the debounce window.
        assert not select.select([began_read], [], [], 0.1)[0]
        term.send(b"/status\r")
        started(began_read)
        os.write(release_write, b"1")
        term.wait(lambda: term.rendered().count(b"Status") >= 2)
        assert not select.select([began_read], [], [], 0.1)[0]
        term.close()
        term = None
        env["BERGET_API_KEY"] = "fixture"
        term = Terminal([str(cli), "-N", "-p", "berget", "-C", str(root)],
                        env, (began_write, release_read))
        term.wait(lambda: term.contains("Welcome to CAI"))
        # No provider catalog or quota request is needed before a prompt.
        assert not select.select([began_read], [], [], 0.1)[0]
    finally:
        try:
            if term is not None:
                if term.process.poll() is not None and term.process.returncode != 0:
                    raise AssertionError(term.process.stderr.read().decode(errors="replace"))
                term.close()
        finally:
            for fd in (began_read, began_write, release_read, release_write):
                os.close(fd)
