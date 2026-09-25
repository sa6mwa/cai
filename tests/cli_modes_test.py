#!/usr/bin/env python3
"""Observable CLI modes against a local Responses stream fixture."""
import http.server
import json
import os
import pathlib
import socketserver
import subprocess
import sys
import tempfile
import threading

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


with tempfile.TemporaryDirectory() as directory:
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
        assert len(server.requests) == 1, server.requests
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
