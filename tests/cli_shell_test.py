#!/usr/bin/env python3
"""Explicit shell commands: pipe streaming, context, replay and literal inputs."""
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix="shell-") as directory:
        root = pathlib.Path(directory)
        work = root / "work"
        work.mkdir()
        api = Responses()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("LOG_") and k.lower() not in
               ("http_proxy", "https_proxy", "all_proxy", "no_color")}
        env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
                   XDG_CONFIG_HOME=str(root / "config"), XDG_CACHE_HOME=str(root / "cache"),
                   XDG_DATA_HOME=str(root / "data"), SHELL="/bin/sh",
                   CAI_API_KEY="fixture", NO_PROXY="127.0.0.1,localhost",
                   no_proxy="127.0.0.1,localhost")
        args = [str(cli), "-p", "custom", "--endpoint",
                f"http://127.0.0.1:{api.server_port}/v1", "-m", "gpt-5.6-luna",
                "--no-terminal", "--no-image-generation", "-C", str(work)]
        terminal = None

        def command(text, expected):
            terminal.output.clear()
            terminal.send(text.encode() + b"\r")
            terminal.wait(lambda: terminal.contains(expected))

        def prompt(text):
            count = len(api.requests)
            terminal.output.clear()
            terminal.send(text.encode() + b"\r")
            terminal.wait(lambda: len(api.requests) > count and terminal.contains("Worked for"))
            return json.dumps(api.requests[-1])

        try:
            terminal = Terminal(args + ["-N"], env)
            terminal.wait(lambda: terminal.contains("Welcome to CAI"))
            command("/goal status", "No goal.")
            assert b"$ /goal status" in terminal.rendered(), terminal.rendered()
            command("/help", "Run cai --help")
            assert b"$ /help" in terminal.rendered()
            assert terminal.contains("shell-context")
            command("/nonesuch", "Unknown command")
            assert not api.requests, "local commands called inference"
            command("!test ! -t 0 && test ! -t 1 && test ! -t 2 && "
                    "cat && printf '# literal **stars** & <tags>\\n' && "
                    "printf stderr-marker\\n >&2; touch ../normal-user-permissions", "Shell command exited 0")
            assert terminal.contains("# literal **stars** & <tags>"), terminal.rendered()
            assert terminal.contains("stderr-marker"), terminal.rendered()
            assert (root / "normal-user-permissions").exists(), "user shell was sandboxed"
            assert not api.requests, "shell started an agent turn"
            request = prompt("use the shell result")
            assert "<user_shell_command>" in request and "Exit code: 0" in request
            assert "literal **stars**" in request and "stderr-marker" in request
            assert "/goal status" not in request and "/help" not in request

            command("/shell-context off", "stay out of model context")
            command("!printf secret-local-result", "Shell command exited 0")
            assert terminal.contains("secret-local-result")
            request = prompt("verify local-only command")
            assert "secret-local-result" not in request
            command("/shell-context on", "shared with the model")
            command("!printf 'nonzero-error\\n' >&2; exit 7", "Shell command exited 7")
            request = prompt("explain the command failure")
            assert "Exit code: 7" in request and "nonzero-error" in request

            # Early bytes are visible and journaled while the process waits;
            # local commands stay responsive, prompts wait for its result.
            gate = work / "gate"
            command("!printf EARLY-shell; while [ ! -f gate ]; do sleep 0.01; done; "
                    "printf LATE-shell", "EARLY-shell")
            assert not gate.exists()
            count = len(api.requests)
            command("/shell-context off", "stay out of model context")
            terminal.send(b"queued behind the shell\r")
            terminal.wait(lambda: terminal.contains("queued behind the shell"))
            assert len(api.requests) == count
            command("!touch never-started", "a shell command is running")
            assert not (work / "never-started").exists()
            gate.touch()
            terminal.wait(lambda: len(api.requests) > count and terminal.contains("Worked for"))
            request = json.dumps(api.requests[-1])
            assert "EARLY-shellLATE-shell" in request, "sharing did not snapshot command start"
            assert "queued behind the shell" in request

            command("/shell-context on", "shared with the model")
            command("!printf CANCELLABLE; sleep 60", "CANCELLABLE")
            terminal.send(b"\x03")
            terminal.wait(lambda: terminal.contains("Shell command exited 137"))
            request = prompt("after cancellation")
            assert "Exit code: 137" in request and "CANCELLABLE" in request
            command("!kill -TERM $$", "Shell command exited 143")
            command("!", "shell command must contain")
            command("!printf '\\000\\033\\377\\342\\202\\254'", "Shell command exited 0")
            assert terminal.contains("\\x00\\x1b\\xff€"), terminal.rendered()
            command("!printf replay-once >> executions; cat executions", "Shell command exited 0")
            command("!python3 -c 'print(\"L\" * 70000); print(\"RAW-END\")'", "Shell command exited 0")
            assert terminal.contains("RAW-END")
            request = prompt("bounded shell context")
            assert "Output truncated for model context;" in request
            assert "\\nRAW-END\\n" not in request, "full shell output was retained for inference"
            terminal.close()
            terminal = None

            # Utilities expose the lossless raw journal; Markdown remains the
            # existing libcai export. Replay never re-executes the command.
            listed = subprocess.run(args + ["--list"], env=env, capture_output=True,
                                    text=True, check=True, timeout=10)
            session_id = listed.stdout.splitlines()[0].split()[0]
            exported = subprocess.run(args + ["--export", session_id], env=env,
                                      capture_output=True, text=True, check=True, timeout=10)
            records = [json.loads(line) for line in
                       (root / "data" / "cai" / "exports" / session_id /
                        f"{session_id}.jsonl").read_text().splitlines()]
            raw = b"".join(bytes.fromhex(record["data"])
                           for record in records if record.get("record_type") == "event" and
                           record["type"] == "user_shell_output_hex")
            assert b"secret-local-result" in raw and b"RAW-END" in raw
            queued = next(record for record in records
                          if record.get("type") == "turn_queued" and
                          record.get("data") == "queued behind the shell")
            completed = next(record for record in records
                             if record.get("type") == "user_shell_completed" and
                             "EARLY-shellLATE-shell" in record.get("data", ""))
            assert queued["sequence"] < completed["sequence"], "queued prompt was not durable while shell ran"
            assert b"\x00\x1b\xff\xe2\x82\xac" in raw
            count = len(api.requests)
            terminal = Terminal(args, env)
            terminal.wait(lambda: terminal.contains("RAW-END") and terminal.contains("Welcome to CAI"))
            assert terminal.contains("secret-local-result"), "unshared output not replayed"
            assert terminal.contains("$ !printf"), terminal.rendered()
            assert (work / "executions").read_text() == "replay-once"
            assert len(api.requests) == count
            request = prompt("after replay")
            assert request.count("<user_shell_command>") == 8, request
            assert "secret-local-result" not in request
            terminal.close()
            terminal = None

            # Startup sharing override and an unavailable shell are independent
            # of the agent's --no-terminal tool flag.
            selected = dict(env, SHELL=str(root / "missing-shell"))
            terminal = Terminal(args + ["-N", "--no-shell-context"], selected)
            terminal.wait(lambda: terminal.contains("Welcome to CAI"))
            command("!echo unavailable", "Shell command exited 127")
            assert terminal.contains("Cannot execute user shell")
            request = prompt("after failed unshared shell")
            assert "<user_shell_command>" not in request
            terminal.close()
            terminal = None
        finally:
            if terminal is not None:
                terminal.close()
            api.release.set()
            api.shutdown()
            api.server_close()
            api.thread.join()


if __name__ == "__main__":
    main()
