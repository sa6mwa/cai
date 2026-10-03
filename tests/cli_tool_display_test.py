#!/usr/bin/env python3
"""Real cai PTY and pslog routing for patches and command previews."""
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


PATCH = """*** Begin Patch
*** Update File: file.c
@@
 before0
 before1
-old
+new
 between0
 between1
 between2
 between3
-old2
+new2
 after0
 after1
*** End Patch
"""
ORIGINAL = "before0\nbefore1\nold\nbetween0\nbetween1\nbetween2\nbetween3\nold2\nafter0\nafter1\n"

def finished(terminal, offset, response, marker=None):
    text = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", terminal.output[offset:])
    return response in text and b"Worked for" in text and (marker is None or marker in text)


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix="tool-display-") as temp:
        root = pathlib.Path(temp)
        workspace = root / "workspace"
        workspace.mkdir()
        target = workspace / "file.c"
        api = Responses()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("LOG_") and k != "NO_COLOR"
               and k.lower() not in ("http_proxy", "https_proxy", "all_proxy")}
        env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
                   XDG_CACHE_HOME=str(root / "cache"), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_DATA_HOME=str(root / "data"), CAI_API_KEY="fixture",
                   NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
        args = [str(cli), "-N", "-p", "custom", "--endpoint",
                f"http://127.0.0.1:{api.server_port}/v1", "-m", "gpt-5.6-luna",
                "--no-image-generation", "-C", str(workspace)]
        terminal = None
        try:
            target.write_text(ORIGINAL)
            api.plan = [("apply_patch", PATCH), "PATCH DONE"]
            terminal = Terminal(args, env)
            terminal.wait(lambda: terminal.contains("> "))
            terminal.send(b"apply supplied patch\r")
            terminal.wait(lambda: terminal.contains("PATCH DONE") and terminal.contains("Worked for"))
            text = terminal.rendered()
            assert b"* Edited file.c" in text, text
            assert re.search(rb"\x1b\[(?:0;)?1;96mEdited", terminal.output)
            assert re.search(rb"\x1b\[(?:0;)?1;96mfile.c", terminal.output)
            assert b"-old" in text and b"+new" in text
            assert b"between1" not in text and b"before0" not in text and b"after1" not in text, text
            assert "⋮".encode() in text
            assert b"\x1b[41m" in terminal.output and b"\x1b[42m" in terminal.output
            assert target.read_text() == ORIGINAL.replace("old\n", "new\n").replace("old2\n", "new2\n")
            request_count = len(api.requests)
            terminal.send(b"/output full\r")
            terminal.wait(lambda: terminal.contains("setting applies from the next tool call"))
            assert len(api.requests) == request_count, "/output must not steer or call the provider"
            target.write_text(ORIGINAL)
            offset = len(terminal.output)
            api.plan = [("apply_patch", PATCH), "FULL PATCH DONE"]
            terminal.send(b"full patch\r")
            terminal.wait(lambda: finished(terminal, offset, b"FULL PATCH DONE"))
            terminal.read()
            later = bytes(terminal.output[offset:])
            assert b"between1" in later and b"before0" in later and b"after1" in later, later
            terminal.send(b"/output compact\r")
            terminal.read()
            command = "printf 'OUT01\\nOUT02\\nOUT03\\nOUT04\\nOUT05\\nOUT06\\nOUT07\\n'"
            # The command's own header contains the text OUT06; only the body
            # must omit that line. Give the shell command a short script path.
            script = workspace / "command.sh"
            script.write_text(command + "\nexit 3\n")
            api.plan = [("exec_command", {"cmd": "sh command.sh", "yield_time_ms": 1000}), "COMMAND DONE"]
            offset = len(terminal.output)
            terminal.send(b"run failing command\r")
            terminal.wait(lambda: finished(terminal, offset, b"COMMAND DONE"))
            terminal.read()
            later = bytes(terminal.output[offset:])
            assert b"OUT05" in later and b"OUT06" not in later, later
            assert re.search(rb"\x1b\[(?:0;)?1;96mRan", later)
            assert re.search(rb"\x1b\[(?:0;)?1;96msh command.sh", later)
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", later)
            assert b"\x1b[91m*\x1b[0m" in later and b"Ran sh command.sh" in plain and b"exit=3" in plain, later
            binary_script = workspace / "binary.sh"
            binary_script.write_text("printf 'UNICODE\\303\\245 BINARY\\377\\000\\302\\200TAIL\\303'\n")
            api.plan = [("exec_command", {"cmd": "sh binary.sh", "yield_time_ms": 1000}), "BINARY DONE"]
            offset = len(terminal.output)
            terminal.send(b"run binary output command\r")
            terminal.wait(lambda: finished(terminal, offset, b"BINARY DONE", b"Ran sh binary.sh"))
            later = bytes(terminal.output[offset:])
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", later)
            assert "UNICODEå".encode() in plain and b"BINARY\\xff?\\xc2\\x80TAIL\\xc3" in plain, plain
            assert b"\x1b[92m*\x1b[0m" in later and b"Ran sh binary.sh" in plain, later
            plain.decode("utf-8")
            terminal.close()
            terminal = None

            target.write_text(ORIGINAL)
            api.plan = [("apply_patch", PATCH), "FINAL ONLY"]
            run = subprocess.run(args + ["-n", "-i", "apply patch"], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            assert run.returncode == 0, run.stderr.decode(errors="replace")
            assert run.stdout.strip() == b"FINAL ONLY", run.stdout
            records = [json.loads(line) for line in run.stderr.splitlines()]
            patch_messages = [r for r in records if r.get("event") == "tool_patch"]
            assert patch_messages, records
            recorded = str(patch_messages)
            assert "between1" in recorded and "before0" in recorded and "after1" in recorded
            assert b"\x1b" not in run.stderr

            api.plan = [("exec_command", {"cmd": "sh command.sh", "yield_time_ms": 1000}), "LAST RESPONSE"]
            run = subprocess.run(args + ["-n", "--tool-output", "compact", "-i", "run command"],
                                 env=env, capture_output=True, timeout=15)
            assert run.returncode == 0 and run.stdout.strip() == b"LAST RESPONSE", run.stderr
            assert b"OUT06" in run.stderr and b"OUT07" in run.stderr
            assert b"\x1b" not in run.stderr
            for line in run.stderr.splitlines():
                json.loads(line)

            api.plan = [("exec_command", {"cmd": "sh binary.sh", "yield_time_ms": 1000}), "BINARY FINAL"]
            run = subprocess.run(args + ["-n", "-i", "run binary"], env=env,
                                 capture_output=True, timeout=15)
            assert run.returncode == 0 and run.stdout.strip() == b"BINARY FINAL", run.stderr
            for line in run.stderr.splitlines():
                json.loads(line)
            assert "UNICODEå".encode() in run.stderr and b"\x1b" not in run.stderr

            bad = subprocess.run([str(cli), "--tool-output", "invalid"], env=env,
                                 capture_output=True, timeout=5)
            assert bad.returncode != 0 and b"compact or full" in bad.stderr
        finally:
            if terminal is not None:
                terminal.process.kill()
                terminal.process.communicate()
                os.close(terminal.master)
            api.shutdown()
            api.server_close()
            api.thread.join(timeout=2)
    print("interactive patches, compact commands, future settings and pslog routing passed")


if __name__ == "__main__":
    main()
