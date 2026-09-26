#!/usr/bin/env python3
"""Observable CLI modes against a local Responses stream fixture."""
import http.server
import json
import os
import pathlib
import pty
import re
import select
import socketserver
import subprocess
import sys
import tempfile
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
        elif text == "__read_file__":
            events = [{"type": "response.output_item.done", "output_index": 0,
                       "item": {"id": "fc_read", "type": "function_call",
                                "call_id": "call_read", "name": "read_file",
                                "arguments": '{"path":"tracked.txt"}'}}]
        else:
            events = [{"type": "response.output_text.delta", "delta": text}]
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
    env = os.environ.copy()
    env.update({"HOME": str(root), "XDG_STATE_HOME": str(root / "state"),
                "XDG_CONFIG_HOME": str(root / "config"),
                "CAI_API_KEY": "fixture", "NO_PROXY": "127.0.0.1,localhost",
                "no_proxy": "127.0.0.1,localhost"})
    for key in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy",
                "https_proxy", "all_proxy"):
        env.pop(key, None)
    base = [CLI, "-p", "custom", "--endpoint",
            f"http://127.0.0.1:{server.server_port}/v1", "-m", "gpt-5.6-luna",
            "--no-terminal", "--no-image-generation", "-C", str(work)]

    def run(*args):
        return subprocess.run(base + list(args), cwd=root, env=env,
                              text=True, capture_output=True, timeout=10)

    def run_interactive(*args):
        master, slave = pty.openpty()
        process = subprocess.Popen(base + list(args), cwd=root, env=env,
                                   stdin=slave, stdout=slave,
                                   stderr=subprocess.PIPE)
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
            rendered = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", bytes(output))
            return (process.returncode, rendered.decode(errors="replace"),
                    stderr.decode(errors="replace"))
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            os.close(master)

    try:
        server.responses[:] = ["first reply", "second reply"]
        completed = run("-Nni", "first task", "-i", "second task")
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
        assert "first reply" in completed.stdout and "second reply" in completed.stdout
        assert len(server.requests) == 2, server.requests
        assert "first task" in server.requests[0]
        assert "second task" in server.requests[1]
        server.requests.clear()

        server.responses[:] = ["resumed reply"]
        resumed = run("-ni", "third task")
        assert resumed.returncode == 0, (resumed.stdout, resumed.stderr)
        assert "second reply" in resumed.stdout and "resumed reply" in resumed.stdout
        assert "third task" in server.requests[0]
        server.requests.clear()

        listed = run("-l")
        assert listed.returncode == 0, listed.stderr
        identifier = listed.stdout.split()[0]
        assert "first task" in listed.stdout and len(listed.stdout.rstrip()) <= 80
        exported = run("--export", identifier)
        assert exported.returncode == 0, exported.stderr
        json_file, markdown_file = map(pathlib.Path, exported.stdout.splitlines())
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
        new_id = imported.stdout.strip()
        assert new_id != identifier
        resumed_list = run("-C", str(imported_directory), "--resume")
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
        key_file.write_bytes(key[:-2] + (b"1" if key[-2:-1] != b"1" else b"2") + key[-1:])
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

        server.responses[:] = ["interactive reply"]
        status_code, terminal_output, terminal_error = run_interactive(
            "-N", "-i", "verify status prefix")
        assert status_code == 0, (terminal_output, terminal_error)
        assert "! Working" in terminal_output, terminal_output
        assert "! Worked for" in terminal_output, terminal_output
        server.requests.clear()

        server.responses[:] = ["working on goal", "__complete_goal__", "goal done"]
        goal = run("-Nn", "-g", "finish fixture")
        assert goal.returncode == 0, (goal.stdout, goal.stderr)
        assert "working on goal" in goal.stdout and "goal done" in goal.stdout
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

        server.responses[:] = ["__http_error__"]
        failed_turn = run("-n", "-i", "should fail", "-i", "must not run")
        assert failed_turn.returncode != 0, (failed_turn.stdout, failed_turn.stderr)
        assert "HTTP 500" in failed_turn.stderr
        assert "fixture error" in failed_turn.stderr
        assert len(server.requests) == 1, server.requests
        server.requests.clear()

        server.responses[:] = ["__api_error__"]
        provider_failure = run("-Nni", "trigger quota error")
        assert provider_failure.returncode != 0
        assert "OpenAI API request failed" in provider_failure.stderr
        assert "HTTP 429" in provider_failure.stderr
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
        assert "[review started]" in reviewed_default.stderr
        assert "[review completed]" in reviewed_default.stderr
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
        assert "[tool] read_file" in reviewed_activity.stderr
        assert "[tool result]" in reviewed_activity.stderr
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
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
