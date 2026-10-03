#!/usr/bin/env python3
"""Rootless Podman Kube services; all disposable project state stays in build/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
STATE = ROOT / "build/devenv"
CHECKOUT = "cai-" + hashlib.sha256(str(ROOT).encode()).hexdigest()[:10]
SERVICES = ("mcp-everything", "searxng", "inspector")
DEFAULT_IMAGES = {
    "mcp-everything": "localhost/cai-mcp-everything:2026.1.26",
    "searxng": "docker.io/searxng/searxng:2026.9.18-c0042add3",
    "inspector": "ghcr.io/modelcontextprotocol/inspector:1.0.1",
}
ENV = {"mcp-everything": "CAI_MCP_EVERYTHING", "searxng": "CAI_SEARXNG",
       "inspector": "CAI_MCP_INSPECTOR"}


def command(*args, capture=False, check=True):
    result = subprocess.run(["podman", *map(str, args)], text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None)
    if check and result.returncode:
        raise RuntimeError(f"podman {' '.join(map(str, args))} failed ({result.returncode})"
                           + (f": {result.stderr.strip()}" if capture else ""))
    return result


def require_podman():
    if not shutil.which("podman"):
        raise RuntimeError("rootless Podman is required; install podman and configure user namespaces")
    if os.geteuid() == 0:
        raise RuntimeError("run the development services as your ordinary user, without sudo")
    if command("info", "--format", "{{.Host.Security.Rootless}}", capture=True).stdout.strip() != "true":
        raise RuntimeError("Podman must run rootlessly")


def config(service):
    path = STATE / f"{service}.json"
    if path.exists():
        return json.loads(path.read_text())
    base = 30000 + int(CHECKOUT[4:], 16) % 9000
    port = os.environ.get(ENV[service] + "_PORT", str(base + SERVICES.index(service)))
    if not port.isdecimal() or not 1024 <= int(port) <= 65535:
        raise RuntimeError(f"{ENV[service]}_PORT must be an unprivileged port (1024..65535)")
    default_image = DEFAULT_IMAGES[service]
    if service == "mcp-everything":
        version = os.environ.get("CAI_MCP_EVERYTHING_VERSION", "2026.1.26")
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?", version):
            raise RuntimeError("CAI_MCP_EVERYTHING_VERSION must be an exact npm version")
        default_image = f"localhost/cai-mcp-everything:{version}"
    image = os.environ.get(ENV[service] + "_IMAGE", default_image)
    if not re.fullmatch(r"[A-Za-z0-9./_:-]+(?::[A-Za-z0-9_.-]+|@sha256:[0-9a-f]{64})", image) or image.endswith(":latest"):
        raise RuntimeError(f"{ENV[service]}_IMAGE requires a pinned tag or sha256 digest")
    url = f"http://127.0.0.1:{port}" + ("/mcp" if service == "mcp-everything" else "")
    return dict(service=service, pod=f"{CHECKOUT}-{service}", port=int(port), image=image, url=url)


def manifest(service):
    return STATE / f"{service}.yaml"


def write_private(path, text):
    """Publish complete files atomically with restrictive permissions from creation."""
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            output.write(text)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def exists(service):
    result = command("pod", "exists", config(service)["pod"], check=False, capture=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(f"unable to inspect {service}: {result.stderr.strip()}")
    return result.returncode == 0


def render(service):
    c = config(service)
    # A running pod's original manifest is the teardown authority.
    if manifest(service).exists() and exists(service):
        for key, suffix in (("port", "_PORT"), ("image", "_IMAGE")):
            override = os.environ.get(ENV[service] + suffix)
            if override and override != str(c[key]):
                raise RuntimeError(f"{service} is already running with {key}={c[key]}; stop it before changing settings")
        return c
    (STATE / f"{service}.json").unlink(missing_ok=True)
    c = config(service)
    for directory in ("state", "tmp"):
        (STATE / directory / service).mkdir(parents=True, exist_ok=True)
    for directory in ("logs", "credentials"):
        (STATE / directory).mkdir(parents=True, exist_ok=True)
    (STATE / "credentials").chmod(0o700)
    secret_path = STATE / "credentials/searxng-secret"
    if not secret_path.exists():
        write_private(secret_path, secrets.token_hex(32))
    values = dict(POD=c["pod"], UID=os.getuid(), GID=os.getgid(), IMAGE=c["image"],
                  PORT=c["port"], URL=c["url"], STATE=str(STATE / "state" / service),
                  TMP=str(STATE / "tmp" / service), CONFIG=str(ROOT / "devenv/searxng"),
                  SECRET=os.environ.get("CAI_SEARXNG_SECRET", secret_path.read_text()))
    documents = (ROOT / "devenv.yaml.in").read_text().split("\n---\n")
    document = next(d for d in documents if d.startswith(f"# service: {service}\n"))
    def substitute(match):
        value = values[match[1]]
        return str(value) if isinstance(value, int) else json.dumps(value)
    document = re.sub(r"@([A-Z_]+)@", substitute, document)
    write_private(manifest(service), document + "\n")
    write_private(STATE / f"{service}.json", json.dumps(c))
    refresh_manifest()
    return c


def refresh_manifest():
    docs = [manifest(s).read_text() for s in SERVICES if manifest(s).exists()]
    if docs:
        write_private(STATE / "devenv.yaml", "---\n".join(docs))


def diagnostics(service):
    c = config(service)
    print(f"service={service} pod={c['pod']} image={c['image']} endpoint={c['url']} manifest={manifest(service)}", file=sys.stderr)
    command("pod", "ps", "--filter", f"name={c['pod']}", check=False)
    if exists(service):
        result = command("pod", "logs", "--tail", "80", c["pod"], capture=True, check=False)
        (STATE / "logs").mkdir(parents=True, exist_ok=True)
        write_private(STATE / f"logs/{service}.log", result.stdout + result.stderr)
        print(result.stdout + result.stderr, file=sys.stderr)


def wait(service):
    c = config(service)
    c["url"] = os.environ.get(ENV[service] + "_BASE_URL", c["url"])
    timeout = float(os.environ.get("CAI_DEV_READY_TIMEOUT", "60"))
    if not 0 < timeout <= 600:
        raise RuntimeError("CAI_DEV_READY_TIMEOUT must be between 0 and 600 seconds")
    deadline = time.monotonic() + timeout
    last = "no response"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    while time.monotonic() < deadline:
        try:
            if service == "inspector":
                command("exec", f"{c['pod']}-inspector", "node", "--version", capture=True)
            else:
                request = urllib.request.Request(c["url"] + ("/" if service == "searxng" else ""))
                if service == "mcp-everything":
                    request = urllib.request.Request(c["url"], data=json.dumps({"jsonrpc": "2.0", "id": 1,
                        "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                        "clientInfo": {"name": "cai-kube-wait", "version": "0.0.0"}}}).encode(),
                        headers={"content-type": "application/json", "accept": "application/json, text/event-stream"})
                with opener.open(request, timeout=min(2, max(.01, deadline - time.monotonic()))) as response:
                    body = response.read(65536)
                    if service == "mcp-everything":
                        data = [line[5:].strip() for line in body.decode().splitlines()
                                if line.startswith("data:") and line[5:].strip()]
                        replies = [json.loads(value) for value in data] if data else [json.loads(body)]
                        if not any(isinstance(reply, dict) and isinstance(reply.get("result"), dict)
                                   and isinstance(reply["result"].get("serverInfo"), dict) for reply in replies):
                            raise RuntimeError("initialize response does not contain serverInfo")
            print(f"{service} is ready at {c['url']}")
            return
        except (OSError, urllib.error.URLError, RuntimeError, ValueError) as error:
            last = str(error)
            time.sleep(min(.2, max(0, deadline - time.monotonic())))
    raise RuntimeError(f"timed out waiting for {service} at {c['url']}: {last}")


def down(service):
    if manifest(service).exists() and exists(service):
        result = command("kube", "down", manifest(service), capture=True, check=False)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if result.returncode or re.search(r"^Error:", result.stdout + result.stderr, re.M):
            raise RuntimeError(f"Podman reported a teardown error for {service}; retained manifest {manifest(service)}")
        if exists(service):
            raise RuntimeError(f"Podman left {service} running; retained manifest {manifest(service)}")


def check_writable_state(service):
    c = config(service)
    target = "/var/cache/searxng" if service == "searxng" else "/state"
    paths = [f"{target}/.cai-owner-check", "/tmp/.cai-owner-check"]
    if service == "searxng":
        program = ["/usr/local/searxng/.venv/bin/python", "-c",
                   "import sys; from pathlib import Path; [Path(p).write_text('cai ownership check') for p in sys.argv[1:]]"]
    else:
        program = ["node", "-e", "process.argv.slice(1).forEach(p => require('fs').writeFileSync(p, 'cai ownership check'))"]
    command("exec", f"{c['pod']}-{service}", *program, *paths)
    for directory in ("state", "tmp"):
        path = STATE / directory / service / ".cai-owner-check"
        if path.stat().st_uid != os.getuid():
            raise RuntimeError(f"{service} wrote {path} as another host UID; fix its user namespace mapping")


def up(services):
    created = []
    visited = []
    try:
        for service in services:
            visited.append(service)
            c = render(service)
            if not exists(service):
                if service == "mcp-everything" and c["image"].startswith("localhost/cai-mcp-everything:"):
                    image_status = command("image", "exists", c["image"], check=False).returncode
                    if image_status not in (0, 1):
                        raise RuntimeError(f"unable to inspect the MCP image ({image_status})")
                    if image_status == 1:
                        command("build", "--tag", c["image"], "--build-arg", f"MCP_EVERYTHING_VERSION={c['image'].split(':')[-1]}",
                                "--file", ROOT / "devenv/mcp-everything/Containerfile", ROOT / "devenv/mcp-everything")
                created.append(service)
                # Forward only the local MCP fixture port into the inspector's
                # loopback interface, without sharing the host network.
                network = ["--network", f"pasta:-T,{c['port']}" if service == "inspector" else "pasta"]
                command("kube", "play", *network, manifest(service))
            wait(service)
            check_writable_state(service)
            print(f"{ENV[service]}_BASE_URL={c['url']} pod={c['pod']} image={c['image']}")
        print(f"state={STATE} credentials={STATE / 'credentials'}")
    except Exception:
        for service in reversed(visited):
            try:
                diagnostics(service)
                if service in created and os.environ.get("CAI_E2E_KEEP_DEVSERVICES") != "1":
                    down(service)
            except Exception as error:
                print(f"cleanup failed for {service}: {error}", file=sys.stderr)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "render", "up", "down", "reset", "ps", "logs", "diagnostics", "exists", "endpoint", "wait", "pull", "exec"))
    parser.add_argument("services", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.action == "exec" and len(args.services) < 2:
        parser.error("exec needs a service and a command")
    services = args.services or list(SERVICES[:2])
    extra = []
    if args.action == "exec":
        services, extra = services[:1], services[1:]
    if any(s not in SERVICES for s in services):
        parser.error("unknown service (use mcp-everything, searxng, inspector)")
    if "inspector" in services and args.action in ("up", "render", "pull", "exec") and os.environ.get("CAI_MCP_INSPECTOR_E2E") != "1":
        parser.error("inspector requires CAI_MCP_INSPECTOR_E2E=1")
    if (ROOT / "build").is_symlink() or STATE.is_symlink() or (STATE.exists() and any(p.is_symlink() for p in STATE.rglob("*"))):
        raise RuntimeError("refusing symlinks in generated build/devenv state")
    if args.action == "endpoint":
        for s in services:
            print(os.environ.get(ENV[s] + "_BASE_URL", config(s)["url"]))
        return
    if args.action == "wait" and "inspector" not in services:
        for s in services:
            wait(s)
        return
    require_podman()
    if args.action == "up":
        up(services)
    elif args.action == "exists":
        sys.exit(0 if all(exists(s) for s in services) else 1)
    elif args.action == "reset":
        for s in SERVICES:
            down(s)
        if STATE.exists():
            shutil.rmtree(STATE)
    elif args.action == "exec":
        if not extra:
            parser.error("exec needs a command")
        command("exec", f"{config(services[0])['pod']}-{services[0]}", *extra)
    else:
        for s in (SERVICES if args.action in ("down", "ps", "logs") and not args.services else services):
            if args.action == "render":
                render(s)
            elif args.action == "down":
                down(s)
            elif args.action == "wait":
                wait(s)
            elif args.action == "diagnostics":
                diagnostics(s)
            elif args.action == "pull":
                command("pull", config(s)["image"])
            elif args.action == "ps":
                command("pod", "ps", "--filter", f"name={config(s)['pod']}")
            elif args.action == "logs" and exists(s):
                command("pod", "logs", config(s)["pod"])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError) as error:
        print(f"devenv: {error}", file=sys.stderr)
        # Reserve 1 for a successful `exists` query reporting an absent pod.
        # Callers must not mistake engine failures for permission to own a pod.
        sys.exit(2)
