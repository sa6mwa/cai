#!/usr/bin/env python3
"""Observable CLI modes against a local Responses stream fixture."""
import fcntl
import http.server
import json
import os
import pathlib
import pty
import re
import select
import socketserver
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

CLI = sys.argv[1]
REPORT = {
    "findings": [{
        "title": "Fix parser",
        "body": "Reject bad input",
        "confidence_score": 0.9,
        "priority": 1,
        "code_location": {
            "absolute_file_path": "/tmp/project/parser.c",
            "line_range": {"start": 4, "end": 6},
        },
    }],
    "overall_correctness": "patch is incorrect",
    "overall_explanation": "One issue found",
    "overall_confidence_score": 0.9,
}


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self, address):
        super().__init__(address, Handler)
        self.responses = []
        self.requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = self.rfile.read(length).decode()
        self.server.requests.append(request)
        text = self.server.responses.pop(0) if self.server.responses else "unexpected request"
        if text == "__http_error__":
            self.send_error(500, "fixture error")
            return
        if text == "__api_error__":
            body = json.dumps({"error": {"message": "quota exhausted",
                                         "type": "rate_limit_error",
                                         "code": "rate_limit_exceeded"}}).encode()
            self.send_response(429)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("x-request-id", "req_fixture_429")
            self.end_headers()
            self.wfile.write(body)
            return
        identifier = f"resp_{len(self.server.requests)}"
        if text == "__complete_goal__":
            events = [{"type": "response.output_item.done", "output_index": 0,
                       "item": {"id": "fc_goal", "type": "function_call",
                                "call_id": "call_goal", "name": "update_goal",
                                "arguments": '{"status":"complete"}'}}]
        elif text == "__run_review__":
            events = [{"type": "response.output_item.done", "output_index": 0,
                       "item": {"id": "fc_review", "type": "function_call",
                                "call_id": "call_review", "name": "run_review",
                                "arguments": '{"target":"uncommitted"}'}}]
        elif text == "__summary__":
            events = [{"type": "response.reasoning_summary_text.delta",
                       "delta": "Short provider summary"},
                      {"type": "response.output_text.delta", "delta": "summary reply"}]
        elif text == "__partial_failure__":
            events = [{"type": "response.output_text.delta", "delta": "partial answer"},
                      {"type": "error", "code": "fixture_error", "message": "stream failed"}]
        elif text == "__read_file__":
            events = [{"type": "response.output_item.done", "output_index": 0,
                       "item": {"id": "fc_read", "type": "function_call",
                                "call_id": "call_read", "name": "read_file",
                                "arguments": '{"path":"tracked.txt"}'}}]
        else:
            events = [{"type": "response.output_text.delta", "delta": text}]
        if text != "__partial_failure__":
            events.append({"type": "response.completed", "response": {
            "id": identifier,
            "usage": {"input_tokens": 5, "output_tokens": 5,
                      "total_tokens": 10},
        }})
        body = "".join(
            f"event: {event['type']}\ndata: {json.dumps(event)}\n\n"
            for event in events
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


with tempfile.TemporaryDirectory(dir=pathlib.Path(CLI).parent) as directory:
    root = pathlib.Path(directory)
    work = root / "work"
    work.mkdir()
    subprocess.run(["git", "init", "-q", str(work)], check=True)
    (work / "tracked.txt").write_text("before\n")
    subprocess.run(["git", "-C", str(work), "add", "tracked.txt"], check=True)
    subprocess.run(["git", "-C", str(work), "-c", "user.name=Fixture",
                    "-c", "user.email=fixture@example.test", "commit", "-qm",
                    "initial"], check=True)
    (work / "tracked.txt").write_text("after\n")
    server = Server(("127.0.0.1", 0))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("LOG_")}
    env.update({"HOME": str(root), "XDG_STATE_HOME": str(root / "state"),
                "XDG_CONFIG_HOME": str(root / "config"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_DATA_HOME": str(root / "data"),
                "CAI_API_KEY": "fixture", "NO_PROXY": "127.0.0.1,localhost",
                "no_proxy": "127.0.0.1,localhost"})
    for key in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy",
                "https_proxy", "all_proxy"):
        env.pop(key, None)
    base = [CLI, "-p", "custom", "--endpoint",
            f"http://127.0.0.1:{server.server_port}/v1", "-m", "gpt-5.6-luna",
            "--no-terminal", "--no-image-generation", "-C", str(work)]

    def run(*args, overrides=None):
        selected_env = env.copy()
        for name, value in (overrides or {}).items():
            if value is None:
                selected_env.pop(name, None)
            else:
                selected_env[name] = value
        return subprocess.run(base + list(args), cwd=root, env=selected_env,
                              text=True, capture_output=True, timeout=10)

    def run_interactive(*args, overrides=None, terminal_stderr=False):
        master, slave = pty.openpty()
        selected_env = env.copy()
        selected_env.update(overrides or {})
        process = subprocess.Popen(base + list(args), cwd=root, env=selected_env,
                                   stdin=slave, stdout=slave,
                                   stderr=slave if terminal_stderr else subprocess.PIPE)
        os.close(slave)
        output = bytearray()
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.1)
                if ready:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output.extend(chunk)
                if b"Worked for" in output:
                    os.write(master, b"/quit\r")
                    break
                if process.poll() is not None:
                    break
            try:
                _, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired as exc:
                raise AssertionError(
                    bytes(output).decode(errors="replace")) from exc
            while select.select([master], [], [], 0)[0]:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                output.extend(chunk)
            rendered = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", bytes(output))
            return (process.returncode, rendered.decode(errors="replace"),
                    stderr.decode(errors="replace") if stderr is not None else "")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            os.close(master)

    try:
        server.responses[:] = ["first reply", "second reply"]
        completed = run("-Nni", "first task", "-i", "second task")
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
        assert "first reply" not in completed.stdout and "second reply" in completed.stdout
        records = [json.loads(line) for line in completed.stderr.splitlines()]
        assert all("app" not in entry for entry in records)
        for role, text in (("user", "first task"), ("assistant", "first reply"),
                           ("user", "second task"), ("assistant", "second reply")):
            assert any(entry.get("lvl") == "info" and entry.get("role") == role
                       and entry.get("prompt_kind") == "normal"
                       and entry.get("msg") == text for entry in records), (role, text)
        assert any(entry.get("msg") == "cai.client.opened" for entry in records)
        assert any(entry.get("sys") == "storage.pouch" for entry in records)
        assert len(server.requests) == 2, server.requests
        assert "first task" in server.requests[0]
        assert "second task" in server.requests[1]
        server.requests.clear()

        server.responses[:] = ["resumed reply"]
        resumed = run("-ni", "third task")
        assert resumed.returncode == 0, (resumed.stdout, resumed.stderr)
        assert "second reply" not in resumed.stdout and "resumed reply" in resumed.stdout
        assert "second reply" in resumed.stderr
        assert "third task" in server.requests[0]
        server.requests.clear()

        listed = run("-l")
        assert listed.returncode == 0, listed.stderr
        assert listed.stderr == "", listed.stderr
        long_list = run("--list")
        assert long_list.returncode == 0 and long_list.stdout == listed.stdout
        assert long_list.stderr == "", long_list.stderr
        for level in ("info", "debug", "trace"):
            verbose_list = run("--list", overrides={"LOG_LEVEL": level})
            assert verbose_list.returncode == 0 and verbose_list.stdout == listed.stdout
            logs = [json.loads(line) for line in verbose_list.stderr.splitlines()]
            assert any(entry.get("lvl") == level for entry in logs), logs
            assert "\x1b" not in verbose_list.stderr
        utility_file = root / "utility.log"
        redirected_list = run("--list", overrides={"LOG_LEVEL": "debug",
                              "LOG_OUTPUT": str(utility_file)})
        assert redirected_list.returncode == 0 and redirected_list.stderr == ""
        assert redirected_list.stdout == listed.stdout
        assert any(entry.get("lvl") == "debug" for entry in
                   map(json.loads, utility_file.read_text().splitlines()))
        for flag in ("--help", "--version"):
            utility = run(flag)
            assert utility.returncode == 0 and utility.stdout and utility.stderr == ""
        bad_option = run("--invalid-option")
        assert bad_option.returncode != 0 and bad_option.stdout == ""
        assert any(entry.get("lvl") == "error" for entry in
                   map(json.loads, bad_option.stderr.splitlines()))
        # Auto color follows the real stderr destination, including JSON output.
        for mode in ("json", "console"):
            terminal_env = env.copy()
            terminal_env.pop("NO_COLOR", None)
            terminal_env["TERM"] = "xterm-256color"
            terminal_env["LOG_MODE"] = mode
            master, slave = pty.openpty()
            try:
                colored = subprocess.run(base + ["--invalid-option"], env=terminal_env,
                                         stdout=subprocess.PIPE, stderr=slave, timeout=10)
                os.close(slave)
                slave = -1
                colored_log = bytearray()
                while select.select([master], [], [], 1)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    colored_log.extend(chunk)
                assert colored.returncode != 0 and colored.stdout == b""
                assert b"\x1b[" in colored_log, (mode, colored_log)
            finally:
                if slave >= 0:
                    os.close(slave)
                os.close(master)
            plain = run("--invalid-option", overrides={"LOG_MODE": mode})
            assert plain.returncode != 0 and "\x1b" not in plain.stderr
        identifier = listed.stdout.split()[0]
        assert "first task" in listed.stdout and len(listed.stdout.rstrip()) <= 80
        exported = run("--export", identifier)
        assert exported.returncode == 0, exported.stderr
        assert exported.stderr == "", exported.stderr
        json_file, markdown_file = map(pathlib.Path, exported.stdout.splitlines())
        assert json_file.parent == root / "data/cai/exports" / identifier
        assert not (root / "state/cai/exports").exists()
        for value in (None, "", "relative-data-home"):
            fallback = run("--export", identifier, overrides={"XDG_DATA_HOME": value})
            assert fallback.returncode == 0, fallback.stderr
            fallback_files = list(map(pathlib.Path, fallback.stdout.splitlines()))
            assert fallback_files[0].parent == root / ".local/share/cai/exports" / identifier
            assert fallback_files[0].read_bytes() == json_file.read_bytes()
        override_export = run("--export", identifier, "--export-dir", "relative-exports")
        assert override_export.returncode == 0, override_export.stderr
        assert (work / "relative-exports" / identifier / f"{identifier}.md").is_file()
        records = [json.loads(line) for line in json_file.read_text().splitlines()]
        assert records[0]["record_type"] == "checkpoint"
        assert records[0]["applied_event_sequence"] > 0
        assert any(record.get("record_type") == "event" for record in records)
        markdown = markdown_file.read_text()
        assert "## User" in markdown and "first task" in markdown
        assert "first reply" in markdown and "resumed reply" in markdown
        assert not server.requests, "export sent a provider request"
        assert (json_file.stat().st_mode & 0o777) == 0o600
        assert (markdown_file.stat().st_mode & 0o777) == 0o600
        imported_directory = root / "imported"
        imported_directory.mkdir()
        imported = run("-C", str(imported_directory), "--import", str(json_file))
        assert imported.returncode == 0, imported.stderr
        assert imported.stderr == "", imported.stderr
        new_id = imported.stdout.strip()
        assert new_id != identifier
        resumed_list = run("-C", str(imported_directory), "--resume")
        assert resumed_list.returncode == 0 and resumed_list.stderr == ""
        assert new_id in resumed_list.stdout and identifier not in resumed_list.stdout
        roundtrip = run("--export", new_id, "--export-dir", str(root / "exports"))
        assert roundtrip.returncode == 0, roundtrip.stderr
        roundtrip_files = list(map(pathlib.Path, roundtrip.stdout.splitlines()))
        assert "resumed reply" in roundtrip_files[1].read_text()
        assert len(roundtrip_files[0].read_text().splitlines()) == len(records)
        assert not server.requests

        before_failed_import = run("-l").stdout
        invalid_inputs = ["{", '{}\n', json.dumps({**records[0], "state": {}}),
                          json.dumps(records[0]) + '\n{"record_type":"event","sequence":0,"type":"bad"}\n',
                          json.dumps(records[0]) + '\n{"record_type":"event","sequence":1,"type":"bad"}\n'
                          '{"record_type":"event","sequence":1,"type":"duplicate"}\n',
                          json_file.read_text() + '{"record_type":"event"']
        for index, invalid in enumerate(invalid_inputs):
            invalid_file = root / f"invalid-{index}.jsonl"
            invalid_file.write_text(invalid)
            failed_import = run("--import", str(invalid_file))
            assert failed_import.returncode != 0, (index, failed_import.stdout)
            assert any(entry.get("lvl") == "error" for entry in
                       map(json.loads, failed_import.stderr.splitlines()))
            assert run("-l").stdout == before_failed_import, "failed import published a session"
        assert not server.requests

        key_file = root / "state" / "cai" / "pouch.key"
        key = key_file.read_bytes()
        assert (key_file.stat().st_mode & 0o777) == 0o600
        key_file.unlink()
        missing_key = run("-l")
        assert missing_key.returncode != 0 and "restore the original key" in missing_key.stderr
        assert not key_file.exists()
        key_file.write_bytes(key)
        key_file.chmod(0o600)
        assert run("-l").returncode == 0
        # Change significant key bits, not unused bits in the final base64url byte.
        key_start = key.index(b":") + 1
        key_file.write_bytes(key[:key_start] +
                             (b"A" if key[key_start:key_start + 1] != b"A" else b"B") +
                             key[key_start + 1:])
        wrong_key = run("-l")
        assert wrong_key.returncode != 0, wrong_key.stdout
        assert key_file.read_bytes() != key, "wrong key was silently replaced"
        key_file.write_bytes(key)
        for stored_file in (root / "state/cai/pouch").rglob("*"):
            if stored_file.is_file():
                assert b"first task" not in stored_file.read_bytes(), stored_file

        custom_root = root / "alternate pouch"
        custom_endpoint = "pouch://" + str(custom_root).replace(" ", "%20")
        selected_store = run("--lockd", custom_endpoint, "--list")
        assert selected_store.returncode == 0, selected_store.stderr
        assert "No conversations." in selected_store.stdout
        assert custom_root.is_dir()
        assert run("-l").stdout == before_failed_import

        server.responses[:] = ["console cache reply"]
        status_code, terminal_output, terminal_error = run_interactive(
            "-N", "-i", "console cache prompt", overrides={"LOG_MODE": "console"},
            terminal_stderr=True)
        assert status_code == 0 and terminal_error == "", (terminal_output, terminal_error)
        console_id = run("--resume").stdout.split()[1]
        console_cache = (root / "cache/cai" / f"{console_id}.log").read_text()
        assert "console cache prompt" in console_cache and "INF" in console_cache
        assert "\x1b" not in console_cache
        server.requests.clear()

        server.responses[:] = ["interactive reply"]
        status_code, terminal_output, terminal_error = run_interactive(
            "-N", "-i", "verify status prefix")
        assert status_code == 0, (terminal_output, terminal_error)
        assert "! Working" in terminal_output, terminal_output
        assert "! Worked for" in terminal_output, terminal_output
        assert terminal_error == "", terminal_error
        assert "cai.client.opened" not in terminal_output
        assert '"sys":"storage.pouch"' not in terminal_output
        active_id = run("--resume").stdout.split()[1]
        log_file = root / "cache/cai" / f"{active_id}.log"
        session_logs = [json.loads(line) for line in log_file.read_text().splitlines()]
        assert any(entry.get("role") == "user" and entry.get("lvl") == "info"
                   and entry.get("msg") == "verify status prefix" for entry in session_logs)
        assert any(entry.get("role") == "assistant" and entry.get("msg") == "interactive reply"
                   for entry in session_logs)
        assert any(entry.get("msg") == "cai.client.opened" for entry in session_logs)
        assert any(entry.get("sys") == "storage.pouch" for entry in session_logs)
        assert all("app" not in entry for entry in session_logs)
        assert (log_file.stat().st_mode & 0o777) == 0o600
        assert not list((root / "cache/cai").glob("startup-*"))
        server.requests.clear()

        server.responses[:] = ["__api_error__"]
        error_code, error_terminal, error_stderr = run_interactive(
            "-N", "-i", "interactive failure")
        assert error_code == 0, (error_terminal, error_stderr)
        assert "[error]" in error_terminal and "quota exhausted" in error_terminal
        assert "req_fixture_429" in error_terminal and error_stderr == ""
        assert "cai.client.opened" not in error_terminal
        server.requests.clear()

        server.responses[:] = ["working on goal", "__complete_goal__", "goal done"]
        goal = run("-Nn", "-g", "finish fixture")
        assert goal.returncode == 0, (goal.stdout, goal.stderr)
        assert "working on goal" not in goal.stdout and "goal done" in goal.stdout
        assert "working on goal" in goal.stderr
        assert len(server.requests) == 3, server.requests
        assert "finish fixture" in server.requests[0]
        assert "Continue pursuing the active goal" in server.requests[1]
        server.requests.clear()

        server.responses[:] = ["__complete_goal__", "review loop done"]
        fixed = run("--review-and-fix")
        assert fixed.returncode == 0, (fixed.stdout, fixed.stderr)
        assert "review loop done" in fixed.stdout
        assert "run_review" in server.requests[0]
        assert "no actionable findings" in server.requests[0]
        server.requests.clear()

        server.responses[:] = ["__run_review__", json.dumps(REPORT),
                              "__complete_goal__", "parent fix complete"]
        child_review = run("--review-and-fix")
        assert child_review.returncode == 0, (child_review.stdout, child_review.stderr)
        assert "parent fix complete" in child_review.stdout
        assert "Fix parser" not in child_review.stdout and "patch is incorrect" not in child_review.stdout
        child_records = [json.loads(line) for line in child_review.stderr.splitlines()]
        assert any(entry.get("parent_tool_call_id") == "call_review" and
                   entry.get("role") == "assistant" for entry in child_records)
        assert "Fix parser" in child_review.stderr
        assert len(server.requests) == 4, server.requests
        server.requests.clear()

        server.responses[:] = ["__summary__"]
        summary = run("-Nni", "summarize", "-I", "developer fixture")
        assert summary.returncode == 0, (summary.stdout, summary.stderr)
        assert "summary reply" in summary.stdout and "Short provider summary" not in summary.stdout
        summary_records = [json.loads(line) for line in summary.stderr.splitlines()]
        assert any(entry.get("role") == "assistant" and entry.get("lvl") == "info" and
                   entry.get("prompt_kind") == "reasoning_summary" and
                   entry.get("msg") == "Short provider summary" for entry in summary_records)
        assert any(entry.get("role") == "developer" and entry.get("lvl") == "info" and
                   entry.get("msg") == "developer fixture" for entry in summary_records)
        server.requests.clear()

        server.responses[:] = ["previous completed answer", "__api_error__"]
        failed_after_reply = run("-Nni", "first succeeds", "-i", "second fails")
        assert failed_after_reply.returncode != 0
        assert "previous completed answer" in failed_after_reply.stdout
        assert "first succeeds" not in failed_after_reply.stdout and "quota exhausted" not in failed_after_reply.stdout
        server.requests.clear()

        server.responses[:] = ["__partial_failure__"]
        partial = run("-Nni", "partial fails")
        assert partial.returncode != 0, partial.stderr
        assert "partial answer" in partial.stdout
        assert any(entry.get("event") == "run_failed" and entry.get("lvl") == "error"
                   for entry in map(json.loads, partial.stderr.splitlines()))
        assert all(json.loads(line) for line in partial.stderr.splitlines())
        server.requests.clear()

        server.responses[:] = ["quiet final"]
        filtered = run("-Nni", "quiet prompt", overrides={"LOG_LEVEL": "warn"})
        assert filtered.returncode == 0 and "quiet final" in filtered.stdout
        assert "quiet prompt" not in filtered.stderr and "quiet final" not in filtered.stderr
        server.requests.clear()

        server.responses[:] = ["console final"]
        console = run("-Nni", "console prompt", overrides={"LOG_MODE": "console", "LOG_NO_COLOR": "true"})
        assert console.returncode == 0 and "console final" in console.stdout
        assert "console prompt" in console.stderr and "INF" in console.stderr
        assert "\x1b" not in console.stderr
        server.requests.clear()

        server.responses[:] = ["relative activity final"]
        relative_log = run("-Nni", "relative activity prompt",
                           overrides={"LOG_OUTPUT": "relative-activity.log"})
        assert relative_log.returncode == 0 and relative_log.stderr == ""
        assert "relative activity final" in relative_log.stdout
        assert (work / "relative-activity.log").is_file()
        assert not (root / "relative-activity.log").exists()
        assert "relative activity prompt" in (work / "relative-activity.log").read_text()
        server.requests.clear()

        redirected_file = root / "redirected.log"
        server.responses[:] = ["redirected final"]
        redirected = run("-Nni", "redirected prompt", overrides={"LOG_OUTPUT": str(redirected_file),
                         "LOG_VERBOSE_FIELDS": "true", "LOG_OUTPUT_FILE_MODE": "0600"})
        assert redirected.returncode == 0 and redirected.stderr == ""
        assert "redirected final" in redirected.stdout
        redirected_records = [json.loads(line) for line in redirected_file.read_text().splitlines()]
        assert any(entry.get("level") == "info" and entry.get("message") == "redirected prompt"
                   and entry.get("role") == "user" for entry in redirected_records)
        assert (redirected_file.stat().st_mode & 0o777) == 0o600
        assert not list((root / "state/cai").glob(".response-*"))
        server.requests.clear()

        server.responses[:] = ["__http_error__"]
        failed_turn = run("-n", "-i", "should fail", "-i", "must not run")
        assert failed_turn.returncode != 0, (failed_turn.stdout, failed_turn.stderr)
        assert "HTTP 500" in failed_turn.stderr
        assert all(json.loads(line) for line in failed_turn.stderr.splitlines())
        assert failed_turn.stdout == ""
        assert "fixture error" in failed_turn.stderr
        assert len(server.requests) == 1, server.requests
        server.requests.clear()

        server.responses[:] = ["__api_error__"]
        provider_failure = run("-Nni", "trigger quota error")
        assert provider_failure.returncode != 0
        assert "OpenAI API request failed" in provider_failure.stderr
        assert "HTTP 429" in provider_failure.stderr
        assert all(json.loads(line) for line in provider_failure.stderr.splitlines())
        assert "rate_limit_exceeded" in provider_failure.stderr
        assert "req_fixture_429" in provider_failure.stderr
        assert "quota exhausted" in provider_failure.stderr
        assert "operation failed" not in provider_failure.stderr
        assert "[error]" not in provider_failure.stdout
        server.requests.clear()

        server.responses[:] = [json.dumps(REPORT)]
        reviewed_default = run("--review")
        assert reviewed_default.returncode == 0, (reviewed_default.stdout, reviewed_default.stderr)
        assert "# Review findings" in reviewed_default.stdout
        assert "patch is incorrect" in reviewed_default.stderr
        assert '"event":"run_started"' in reviewed_default.stderr
        assert '"event":"run_completed"' in reviewed_default.stderr
        assert "staged, unstaged, and untracked" in server.requests[0].lower()
        server.requests.clear()

        server.responses[:] = [json.dumps(REPORT)]
        reviewed = run("--review", "-o", "findings.md")
        assert reviewed.returncode == 0, (reviewed.stdout, reviewed.stderr)
        assert reviewed.stdout == "", reviewed.stdout
        findings = (work / "findings.md").read_text()
        assert "# Review findings" in findings and "## Fix parser" in findings
        assert "patch is incorrect" in reviewed.stderr, reviewed.stderr
        server.requests.clear()

        server.responses[:] = [json.dumps(REPORT)]
        reviewed_base = run("--review", "--base", "HEAD")
        assert reviewed_base.returncode == 0, (reviewed_base.stdout, reviewed_base.stderr)
        assert "HEAD" in server.requests[0], server.requests[0]
        server.requests.clear()

        server.responses[:] = [json.dumps(REPORT)]
        reviewed_custom = run("--review", "-i", "Check the parser only")
        assert reviewed_custom.returncode == 0, (reviewed_custom.stdout, reviewed_custom.stderr)
        assert "Check the parser only" in server.requests[0], server.requests[0]
        server.requests.clear()

        server.responses[:] = ["__read_file__", json.dumps(REPORT)]
        reviewed_activity = run("--review")
        assert reviewed_activity.returncode == 0, (reviewed_activity.stdout, reviewed_activity.stderr)
        assert '"event":"tool_call_started"' in reviewed_activity.stderr
        assert '"tool":"read_file"' in reviewed_activity.stderr
        assert '"event":"tool_call_completed"' in reviewed_activity.stderr
        assert len(server.requests) == 2
        server.requests.clear()

        server.responses[:] = [json.dumps(REPORT)]
        reviewed_json = run("--review", "-T", "json")
        assert reviewed_json.returncode == 0, (reviewed_json.stdout, reviewed_json.stderr)
        assert json.loads(reviewed_json.stdout) == REPORT
        server.requests.clear()

        server.responses[:] = ["not a review report"]
        failed = run("--review")
        assert failed.returncode != 0 and failed.stdout == "", (failed.stdout, failed.stderr)
        server.requests.clear()
        markdown_fixture = ("# Renderer fixture\n\n**UTF-8**: café λ → "
                            "[link](https://example.test)\n\n" +
                            "wrap-fixture " * 100 +
                            "\n\n```text\n\x1b[31mansi-coded\x1b[0m\n```\n")
        server.responses[:] = [markdown_fixture]
        pipe_rendered = run("-Nni", "render to pipe", overrides={"COLUMNS": "120"})
        assert pipe_rendered.returncode == 0, pipe_rendered.stderr
        assert "\x1b" not in pipe_rendered.stdout, pipe_rendered.stdout
        assert "café λ →" in pipe_rendered.stdout and "ansi-coded" in pipe_rendered.stdout
        wrapped = [line for line in pipe_rendered.stdout.splitlines() if "wrap-fixture" in line]
        assert len(wrapped) > 1 and max(map(len, wrapped)) <= 80, wrapped
        server.requests.clear()

        server.responses[:] = [markdown_fixture]
        rendered_path = root / "rendered.txt"
        file_env = env.copy()
        file_env["COLUMNS"] = "120"
        with rendered_path.open("w") as destination:
            file_rendered = subprocess.run(base + ["-Nni", "render to file"], env=file_env,
                                           stdout=destination, stderr=subprocess.PIPE,
                                           text=True, timeout=10)
        assert file_rendered.returncode == 0, file_rendered.stderr
        assert rendered_path.read_text() == pipe_rendered.stdout
        server.requests.clear()

        server.responses[:] = [markdown_fixture]
        master, slave = pty.openpty()
        try:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 120, 0, 0))
            terminal_rendered = subprocess.run(base + ["-Nni", "render to terminal"], env=env,
                                              stdin=subprocess.DEVNULL, stdout=slave,
                                              stderr=subprocess.PIPE, timeout=10)
            os.close(slave)
            slave = -1
            terminal_bytes = bytearray()
            while select.select([master], [], [], 1)[0]:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                terminal_bytes.extend(chunk)
            assert terminal_rendered.returncode == 0, terminal_rendered.stderr
            assert b"\x1b[" in terminal_bytes, terminal_bytes
            terminal_text = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", bytes(terminal_bytes)).decode()
            wrapped = [line for line in terminal_text.splitlines() if "wrap-fixture" in line]
            assert 80 < max(map(len, wrapped)) <= 120, wrapped
        finally:
            if slave >= 0:
                os.close(slave)
            os.close(master)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
