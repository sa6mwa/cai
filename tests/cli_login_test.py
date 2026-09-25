#!/usr/bin/env python3
"""Exercise the CLI's loopback OAuth callback without real credentials."""
import os
import pathlib
import re
import select
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

cli = sys.argv[1]
with tempfile.TemporaryDirectory() as root:
    root = pathlib.Path(root)
    (root / "bin").mkdir()
    opener = root / "bin" / "xdg-open"
    opener.write_text("#!/bin/sh\nexit 0\n")
    opener.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = str(root / "bin") + os.pathsep + env.get("PATH", "")
    env["XDG_STATE_HOME"] = str(root / "state")
    env["HOME"] = str(root)
    proc = subprocess.Popen([cli, "--login"], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env)
    try:
        output = b""
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            ready, _, _ = select.select([proc.stderr], [], [], 0.1)
            if not ready:
                continue
            chunk = os.read(proc.stderr.fileno(), 4096)
            if not chunk:
                break
            output += chunk
            if b"Waiting for OAuth callback" in output:
                break
        output = output.decode()
        match = re.search(r"Waiting for OAuth callback on (http://localhost:\d+/auth/callback)", output)
        assert match, output
        try:
            urllib.request.urlopen(match.group(1).replace("/auth/callback", "/wrong"), timeout=2)
            raise AssertionError("wrong callback path was accepted")
        except urllib.error.HTTPError as exc:
            assert exc.code == 404, exc.code
        try:
            urllib.request.urlopen(match.group(1) + "?error=access_denied", timeout=2)
            raise AssertionError("OAuth failure callback was accepted")
        except urllib.error.HTTPError as exc:
            assert exc.code == 400, exc.code
        assert proc.wait(timeout=3) == 1, output
        assert not (root / "state" / "cai" / "auth.json").exists()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
