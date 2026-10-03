#!/usr/bin/env python3
"""Goal slash commands through the real TUI, local API and Pouch store."""
import json
import os
import pathlib
import sys
import tempfile

sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix="goals-") as directory:
        root = pathlib.Path(directory)
        work = root / "work"
        work.mkdir()
        api = Responses()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("LOG_") and k.lower() not in
               ("http_proxy", "https_proxy", "all_proxy", "no_color")}
        env.update(HOME=str(root), XDG_STATE_HOME=str(root / "state"),
                   XDG_CONFIG_HOME=str(root / "config"), XDG_CACHE_HOME=str(root / "cache"),
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

        try:
            terminal = Terminal(args + ["-N"], env)
            terminal.wait(lambda: terminal.contains("Welcome to CAI"))
            for text in ("/goal", "/goal status"):
                command(text, "No goal.")
            command("/goal help", "Prefer asking the agent to create and manage goals.")
            assert "| --- |" not in terminal.rendered().decode()
            for text in ("/goal new", "/goal new   ", "/goal nonsense",
                         "/goal finish this project", "/goal pause extra"):
                command(text, "* ERROR: input: Use /goal")
            command("/goal pause", "* ERROR: input: only an active goal can pause")
            command("/goal resume", "* ERROR: input: goal is not resumable")
            command("/goal clear", "Goal control queued")
            command("/goal status", "No goal.")
            assert not api.requests, "local goal commands sent a model request"

            # Replacement is accepted while a response is streaming. The
            # current response completes, and its continuation sees the new
            # objective with the same conversation and no intermediate clear.
            api.release.clear()
            api.pause_script = True
            api.plan = ["FIRST pass", ("update_goal", {"status": "complete"}), "REPLACEMENT done"]
            command("/goal new Initial objective", "STREAM in flight")
            command("/goal status", "Token budget: unbounded.")
            assert "Initial objective" in terminal.rendered().decode()
            assert "unbounded" in terminal.rendered().decode()
            command("/goal new Replacement objective", "Goal control queued")
            command("/goal help", "Goal commands")  # input remains responsive
            assert len(api.requests) == 1
            api.release.set()
            terminal.wait(lambda: terminal.contains("REPLACEMENT done") and
                          terminal.contains("Worked for"))
            assert len(api.requests) == 3, api.requests
            assert "Replacement objective" in json.dumps(api.requests[1])
            assert "Initial objective" in json.dumps(api.requests[1])
            assert "/goal new" not in json.dumps(api.requests)
            command("/goal status", "Token budget: unbounded.")
            assert "Replacement objective" in terminal.rendered().decode()
            assert "complete" in terminal.rendered().decode()
            count = len(api.requests)
            command("/goal resume", "* ERROR: input: goal is not resumable")
            assert len(api.requests) == count

            # A fresh goal can replace a completed one. Pausing during the
            # request stops pursuit at its boundary without another request.
            api.release.clear()
            api.pause_script = True
            api.plan = ["PAUSE boundary"]
            command("/goal new Pausable objective", "STREAM in flight")
            command("/goal pause", "Goal control queued")
            count = len(api.requests)
            api.release.set()
            terminal.wait(lambda: terminal.contains("Worked for"))
            command("/goal status", "Token budget: unbounded.")
            assert "Pausable objective" in terminal.rendered().decode()
            assert "paused" in terminal.rendered().decode()
            assert len(api.requests) == count
            terminal.close()
            terminal = Terminal(args, env)
            terminal.wait(lambda: terminal.contains("Welcome to CAI"))
            command("/goal status", "Token budget: unbounded.")
            assert "Pausable objective" in terminal.rendered().decode()
            assert "paused" in terminal.rendered().decode()
            assert len(api.requests) == count, "replay requested inference"
            api.plan = [("update_goal", {"status": "complete"}), "RESUMED done"]
            command("/goal resume", "RESUMED done")
            terminal.wait(lambda: terminal.contains("Worked for"))
            command("/goal status", "Token budget: unbounded.")
            assert "Pausable objective" in terminal.rendered().decode()
            assert "complete" in terminal.rendered().decode()

            # Clear while sampling removes tracking but lets that user turn
            # finish. Clearing again is local and idempotent.
            api.release.clear()
            api.pause_script = True
            api.plan = ["CLEARED partial response", "CLEARED turn completed"]
            command("/goal new Clearable objective", "STREAM in flight")
            command("/goal clear", "Goal control queued")
            count = len(api.requests)
            api.release.set()
            terminal.wait(lambda: terminal.contains("CLEARED turn completed") and
                          terminal.contains("Worked for"))
            assert len(api.requests) == count + 1, "clear did not continue the current turn"
            count = len(api.requests)
            command("/goal status", "No goal.")
            command("/goal clear", "Goal control queued")
            command("/goal", "No goal.")
            assert len(api.requests) == count
            terminal.close()
            terminal = None

            records = [json.loads(line) for file in (root / "cache" / "cai").glob("*.log")
                       for line in file.read_text().splitlines()]
            controls = [r for r in records if r.get("event") == "goal_control"]
            assert len(controls) == 9, controls
            assert all(r["lvl"] == "info" and r["role"] == "user" and
                       r["prompt_kind"] == "goal_control" for r in controls)
            assert {r["command"] for r in controls} == {"new", "pause", "resume", "clear"}
            print("goal commands: local errors/help, mid-turn replacement, completion, pause, replay, resume, clear and logs passed")
        finally:
            api.release.set()
            if terminal is not None:
                terminal.close()
            api.shutdown()
            api.server_close()


if __name__ == "__main__":
    main()
