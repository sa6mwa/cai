#!/usr/bin/env python3
"""Prove live heading/body routing with real softline and libmdf over a PTY."""
import os
import pathlib
import re
import sys
import tempfile
sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix="reasoning-") as temp:
        root = pathlib.Path(temp)
        workspace = root / "workspace"
        workspace.mkdir()
        api = Responses()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("LOG_") and key.lower() not in
               ("http_proxy", "https_proxy", "all_proxy", "no_color")}
        env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
                   XDG_CACHE_HOME=str(root / "cache"), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_DATA_HOME=str(root / "data"), CAI_API_KEY="fixture",
                   NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
        args = [str(cli), "-N", "-p", "custom", "--endpoint",
                f"http://127.0.0.1:{api.server_port}/v1", "-m", "gpt-5.6-luna",
                "--no-image-generation", "-C", str(workspace)]
        terminal = None
        try:
            terminal = Terminal(args, env)
            terminal.wait(lambda: terminal.contains("> "))
            for parts, heading, body in [
                (["**Applying ", "patch**"], "Applying patch", None),
                (["**Inspecting script**\r", "\n\n**Body detail**\n\n",
                  "Further detail with `code`.\n\n"], "Inspecting script", "Body detail"),
                (["# New section\n\n", "A long reasoning body " + "word " * 50 + "\n\n"],
                 "# New section", "A long reasoning body")]:
                api.reasoning = parts
                api.pause = True
                api.release.clear()
                offset = len(terminal.output)
                terminal.send(b"continue\r")
                def heading_visible():
                    recent = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"",
                                    terminal.output[offset:])
                    return b"EARLY paragraph." in recent and re.search(
                        rb"! " + re.escape(heading.encode()) + rb" \(\d+m \d+s\)",
                        recent) is not None
                terminal.wait(heading_visible)
                current = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", terminal.output[offset:])
                assert b"[reasoning]" not in current and b"? " + heading.encode() not in current, current
                assert b"**" + heading.encode() not in current, current
                if body:
                    assert b"? " + body.encode() in current, current
                    assert b"? EARLY paragraph" not in current, current
                    if heading == "Inspecting script":
                        assert b"? Further detail with code." in current, current
                        assert b"**Body detail**" not in current, current
                    else:
                        assert current.count(b"? ") >= 3, current  # wrapped rows carry the prefix
                else:
                    assert b"? " not in current, current
                api.release.set()
                terminal.wait(lambda: b"Worked for" in re.sub(
                    rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", terminal.output[offset:]))
                terminal.read()
            terminal.close()
            terminal = None
        finally:
            api.release.set()
            if terminal is not None:
                if terminal.process.poll() is None:
                    terminal.process.kill()
                terminal.process.communicate()
                os.close(terminal.master)
            api.shutdown()
            api.server_close()
            api.thread.join(timeout=2)


if __name__ == "__main__":
    main()
