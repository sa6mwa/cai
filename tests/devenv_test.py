#!/usr/bin/env python3
"""Black-box lifecycle tests with an isolated Podman process fixture and real HTTP."""
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

repo = Path(sys.argv[1]).resolve()
profile = sys.argv[2] if len(sys.argv) > 2 else "controller"

FAKE = r'''#!/bin/bash
set -eu
printf '%s\t' "$@" >>"$CAI_FAKE_ROOT/events.tsv"
printf '\n' >>"$CAI_FAKE_ROOT/events.tsv"
pods="$CAI_FAKE_ROOT/pods.txt"
touch "$pods"
fail=${CAI_FAKE_FAIL:-}
case "$1:${2:-}" in
 info:*)
  if [[ "$fail" == rootful ]]; then echo false; else echo true; fi
  [[ "$fail" != info ]] || exit 125 ;;
 pod:exists)
  [[ "$fail" != exists ]] || exit 125
  if [[ "$fail" == exists-once && ! -f "$CAI_FAKE_ROOT/exists-failed" ]]; then
   touch "$CAI_FAKE_ROOT/exists-failed"
   exit 125
  fi
  grep -Fxq "$3" "$pods" ;;
 image:exists) if [[ "$fail" == image ]]; then exit 125; else exit 1; fi ;;
 build:*) [[ "$fail" != build ]] || exit 7 ;;
 kube:play|kube:down)
  name=$(sed -n 's/^  name: "\([a-z0-9-]*\)"$/\1/p' "${!#}")
  if [[ "$2" == play ]]; then
   printf '%s\n' "$name" >>"$pods"
   [[ "$fail" != play ]] || exit 7
  else
   [[ "$fail" != down ]] || exit 7
   [[ "$fail" != down-survives ]] || exit 0
   grep -Fvx "$name" "$pods" >"$pods.next" || true
   mv "$pods.next" "$pods"
   if [[ "$fail" == down-reported ]]; then echo 'Error: fixture network cleanup failed' >&2; fi
  fi ;;
 pod:logs) echo 'fixture service diagnostic' ;;
 exec:*)
  [[ "$fail" != exec ]] || exit 7
  if [[ "$*" == *'.cai-owner-check'* ]]; then
   case "$2" in
    *-mcp-everything-mcp-everything) service=mcp-everything ;;
    *-searxng-searxng) service=searxng ;;
    *-inspector-inspector) service=inspector ;;
    *) exit 7 ;;
   esac
   for kind in state tmp; do
    printf 'written by the fixture container' >"$CAI_FAKE_CHECKOUT/build/devenv/$kind/$service/.cai-owner-check"
   done
  fi
  echo v22.22.0 ;;
esac
'''


class Handler(http.server.BaseHTTPRequestHandler):
    bad = False
    sse = False

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.send_response(200)
        self.end_headers()
        body = b'{"error":"not ready"}' if self.bad else b'{"result":{"serverInfo":{"name":"fixture"}}}'
        if self.sse:
            body = b'id: priming\ndata: \n\nevent: message\ndata: ' + body + b'\n\n'
        self.wfile.write(body)

    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ready")

    def log_message(self, *_):
        pass


def executable(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    path.chmod(0o755)


with tempfile.TemporaryDirectory(prefix="devenv-test-", dir=repo / "build") as tmp:
    base = Path(tmp)
    # Include spaces and quotes to exercise YAML path escaping.
    root = base / 'checkout with "quotes"'
    shutil.copytree(repo / "scripts", root / "scripts")
    shutil.copytree(repo / "devenv", root / "devenv")
    shutil.copy(repo / "devenv.yaml.in", root / "devenv.yaml.in")
    executable(base / "bin/podman", FAKE)
    for tool in ("make", "cmake"):
        passthrough = f'if [ "$1" = -E ]; then exec "{shutil.which("cmake")}" "$@"; fi\n' if tool == "cmake" else ""
        executable(base / f"bin/{tool}", '#!/bin/sh\n' + passthrough + 'printf "%s\\n" "$*" >> "$CAI_FAKE_ROOT/build-events"\n')
    executable(root / "build/debug/cai_mcp_everything_e2e", '''#!/bin/sh
printf '%s|%s|%s\n' "$*" "$CAI_MCP_EVERYTHING_BASE_URL" "${CAI_INTEGRATION_CHATGPT_MCP_CLIENT_TOOL:-}" >>"$CAI_FAKE_ROOT/test-events"
exit "${CAI_FAKE_TEST_EXIT:-0}"
''')
    env = {k: v for k, v in os.environ.items() if not k.startswith("CAI_") and k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
    env.update(PATH=f"{base / 'bin'}:{os.environ['PATH']}", CAI_FAKE_ROOT=str(base), CAI_FAKE_CHECKOUT=str(root), CAI_DEV_READY_TIMEOUT=".25")
    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    env["CAI_MCP_EVERYTHING_PORT"] = env["CAI_SEARXNG_PORT"] = str(server.server_port)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .01}, daemon=True)
    thread.start()
    state = root / "build/devenv"

    def run(action, *args, ok=True, **overrides):
        result = subprocess.run([str(root / "scripts/devenv.sh"), action, *args], cwd=base,
                                env=dict(env, **overrides), capture_output=True, text=True, timeout=8)
        assert (result.returncode == 0) == ok, (action, args, result.stdout, result.stderr)
        return result

    def events():
        return [line.split("\t")[:-1] for line in (base / "events.tsv").read_text().splitlines()] if (base / "events.tsv").exists() else []

    def clear():
        (base / "events.tsv").write_text("")

    def pods():
        return (base / "pods.txt").read_text().splitlines() if (base / "pods.txt").exists() else []

    def wrapper(script="test-e2e.sh", ok=True, **overrides):
        args = [str(root / "scripts" / script)]
        if script != "test-e2e.sh":
            args.append(str(root / "build/debug/cai_mcp_everything_e2e"))
        result = subprocess.run(args, cwd=base, env=dict(env, **overrides), capture_output=True, text=True, timeout=8)
        assert (result.returncode == 0) == ok, (script, result.stdout, result.stderr)
        return result

    try:
        if profile == "controller":
            Handler.sse = True
            missing = subprocess.run([sys.executable, str(root / "scripts/devenv.py"), "check"],
                                     env=dict(env, PATH=""), capture_output=True, text=True)
            assert missing.returncode and "rootless Podman is required" in missing.stderr
            run("exec", ok=False)
            run("exec", "inspector", ok=False)
            for overrides, message in [({"CAI_FAKE_FAIL": "rootful"}, "rootlessly"),
                                       ({"CAI_FAKE_FAIL": "info"}, "podman info"),
                                       ({"CAI_MCP_EVERYTHING_PORT": "0"}, "unprivileged port"),
                                       ({"CAI_MCP_EVERYTHING_PORT": "abc"}, "unprivileged port"),
                                       ({"CAI_SEARXNG_IMAGE": "example:latest"}, "pinned tag"),
                                       ({"CAI_SEARXNG_IMAGE": "example"}, "pinned tag"),
                                       ({"CAI_MCP_EVERYTHING_VERSION": "latest"}, "exact npm version")]:
                service = "searxng" if "CAI_SEARXNG_IMAGE" in overrides else "mcp-everything"
                assert message in run("up", service, ok=False, **overrides).stderr
                assert not pods()
            assert "requires CAI_MCP_INSPECTOR_E2E=1" in run("up", "inspector", ok=False).stderr
            run("render")
            document = (state / "devenv.yaml").read_text()
            assert '@PORT@' not in document and 'hostIP: 127.0.0.1' in document
            assert json.dumps(str(root / "devenv/searxng")) in document
            assert 'mountPath: /etc/searxng, readOnly: true' in document
            assert (state / "devenv.yaml").stat().st_mode & 0o777 == 0o600
            assert (state / "credentials/searxng-secret").stat().st_mode & 0o777 == 0o600
            assert (state / "credentials").stat().st_mode & 0o777 == 0o700
            assert 'inspector' not in document
            run("render", "searxng", CAI_SEARXNG_IMAGE="localhost:5000/searxng:2026.9")
            assert 'localhost:5000/searxng:2026.9' in (state / "searxng.yaml").read_text()
            run("up")
            assert len(pods()) == 2
            original = (state / "mcp-everything.yaml").read_bytes()
            clear()
            run("up")
            assert not any(e[:2] == ["kube", "play"] for e in events())
            for service in ("mcp-everything", "searxng"):
                assert (state / f"state/{service}/.cai-owner-check").stat().st_uid == os.getuid()
            changed = run("up", "mcp-everything", ok=False, CAI_MCP_EVERYTHING_PORT="45678")
            assert "stop it before changing" in changed.stderr
            assert (state / "mcp-everything.yaml").read_bytes() == original
            assert not any(e[:2] == ["kube", "down"] for e in events())
            run("reset", ok=False, CAI_FAKE_FAIL="down")
            assert state.exists() and len(pods()) == 2
            run("reset", ok=False, CAI_FAKE_FAIL="down-survives")
            assert state.exists() and len(pods()) == 2
            run("reset", ok=False, CAI_FAKE_FAIL="down-reported")
            assert state.exists(), "reported teardown errors must retain recovery state"
            run("reset")
            assert not state.exists() and not pods() and (root / "devenv/searxng/settings.yml").exists()
            # Partial startup, image build, ownership probe, and readiness failures.
            for fail in ("build", "image", "play", "exec", "exists"):
                clear()
                run("up", "mcp-everything", ok=False, CAI_FAKE_FAIL=fail)
                if fail in ("play", "exec"):
                    assert any(e[:2] == ["kube", "down"] for e in events())
                assert not pods()
                run("reset")
            Handler.bad = True
            clear()
            result = run("up", "mcp-everything", ok=False)
            assert "timed out" in result.stderr and "fixture service diagnostic" in result.stderr
            assert (state / "logs/mcp-everything.log").exists() and not pods()
            run("reset")
            run("up", "mcp-everything", ok=False, CAI_E2E_KEEP_DEVSERVICES="1")
            assert pods()
            clear()
            run("up", "mcp-everything", ok=False)
            assert not any(e[:2] == ["kube", "down"] for e in events()), "must preserve a pre-existing pod"
            Handler.bad = False
            run("reset")
            state.mkdir(parents=True)
            (state / "credentials").symlink_to(root / "devenv")
            assert "refusing symlinks" in run("render", ok=False).stderr
            (state / "credentials").unlink()
            run("reset")
            run("up", "inspector", CAI_MCP_INSPECTOR_E2E="1")
            assert any(e[:3] == ["kube", "play", "--network"] and e[3].startswith("pasta:-T,") for e in events())
            run("reset")
            run("up", "mcp-everything")
            # make clean must stop pods before deleting their teardown manifests.
            clean = root / "scripts/clean.sh"
            failed = subprocess.run([str(clean)], env=dict(env, CAI_FAKE_FAIL="down"), capture_output=True, text=True)
            assert failed.returncode and state.exists() and pods()
            result = subprocess.run([str(clean)], env=env, capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            assert not (root / "build").exists() and not pods()
            assert (root / "devenv/searxng/settings.yml").exists()
        else:
            script = "test-e2e.sh" if profile == "ownership" else "run_mcp_everything_live_test.sh"
            wrapper(script)
            assert not pods(), "owned pod must be stopped after success"
            clear()
            run("up", "mcp-everything")
            clear()
            wrapper(script)
            assert pods() and not any(e[:2] == ["kube", "down"] for e in events())
            if profile == "ownership":
                clear()
                wrapper(script, ok=False, CAI_FAKE_FAIL="exists-once")
                assert pods() and not any(e[:2] == ["kube", "down"] for e in events()), "engine errors must not grant ownership"
            run("reset")
            result = wrapper(script, ok=False, CAI_FAKE_TEST_EXIT="7")
            assert "MCP test failed (7)" in result.stderr and "endpoint=" in result.stderr
            assert not pods()
            run("reset")
            wrapper(script, CAI_E2E_KEEP_DEVSERVICES="1")
            assert pods()
            run("reset")
            clear()
            external = f"http://127.0.0.1:{server.server_port}/external/mcp"
            wrapper(script, CAI_MCP_EVERYTHING_BASE_URL=external)
            assert not events(), "external endpoint must not require Podman"
            report = (base / "test-events").read_text()
            assert external in report
            if profile == "live-wrapper":
                assert '|1\n' in report, "live opt-in must reach the integration binary"
            clear()
            wrapper(script, ok=False, CAI_FAKE_FAIL="play")
            assert not pods()
    finally:
        server.shutdown()
        thread.join()
        server.server_close()
print(f"Podman Kube {profile} checks passed")
