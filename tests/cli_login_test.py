#!/usr/bin/env python3
"""Exercise the CLI's loopback OAuth callback without real credentials."""
import json
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
with tempfile.TemporaryDirectory(dir=pathlib.Path(cli).parent) as root:
    root = pathlib.Path(root)
    (root / "bin").mkdir()
    opener = root / "bin" / "xdg-open"
    opener.write_text("#!/bin/sh\nexit 0\n")
    opener.chmod(0o755)
    for level in (None, "debug"):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("LOG_")}
        env["PATH"] = str(root / "bin") + os.pathsep + env.get("PATH", "")
        env["XDG_STATE_HOME"] = str(root / ("state" if level is None else "debug-state"))
        env["HOME"] = str(root)
        if level:
            env["LOG_LEVEL"] = level
        proc = subprocess.Popen([cli, "--login"], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=env)
        try:
            output = b""
            diagnostics = b""
            streams = [proc.stdout, proc.stderr]
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                ready, _, _ = select.select(streams, [], [], 0.1)
                for stream in ready:
                    chunk = os.read(stream.fileno(), 4096)
                    if not chunk:
                        streams.remove(stream)
                    elif stream is proc.stdout:
                        output += chunk
                    else:
                        diagnostics += chunk
                if b"Waiting for OAuth callback" in output or not streams:
                    break
            match = re.search(rb'Waiting for OAuth callback on (http://localhost:\d+/auth/callback)', output)
            assert match, (output, diagnostics)
            assert b"Open this URL to authenticate:\nhttps://" in output, output
            redirect_uri = match.group(1).decode()
            try:
                urllib.request.urlopen(redirect_uri.replace("/auth/callback", "/wrong"), timeout=2)
                raise AssertionError("wrong callback path was accepted")
            except urllib.error.HTTPError as exc:
                assert exc.code == 404, exc.code
            try:
                urllib.request.urlopen(redirect_uri + "?error=access_denied", timeout=2)
                raise AssertionError("OAuth failure callback was accepted")
            except urllib.error.HTTPError as exc:
                assert exc.code == 400, exc.code
            remaining_output, remaining_diagnostics = proc.communicate(timeout=3)
            output += remaining_output
            diagnostics += remaining_diagnostics
            assert proc.returncode == 1, (output, diagnostics)
            records = [json.loads(line) for line in diagnostics.splitlines()]
            assert any(record.get("lvl") == "error" for record in records), records
            if level is None:
                assert all(record.get("lvl") in ("warn", "error", "fatal", "panic")
                           for record in records), records
            else:
                assert any(record.get("lvl") == "debug" for record in records), records
                assert any(record.get("redirect_uri") == redirect_uri and
                           record.get("lvl") == "info" for record in records), records
            assert not (pathlib.Path(env["XDG_STATE_HOME"]) / "cai/auth.json").exists()
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
