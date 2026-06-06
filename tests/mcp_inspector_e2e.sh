#!/bin/sh
set -eu

if [ "${CAI_MCP_INSPECTOR_E2E:-0}" != "1" ]; then
  echo "SKIP: set CAI_MCP_INSPECTOR_E2E=1 to run MCP Inspector e2e"
  exit 0
fi

if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/cai_mcp_http_server" >&2
  exit 2
fi

server=$1
tmpdir=${TMPDIR:-/tmp}/cai-mcp-inspector-$$
fifo=$tmpdir/port.fifo
list_out=$tmpdir/tools-list.json
call_out=$tmpdir/tools-call.json
server_pid=
runtime=
image=${CAI_MCP_INSPECTOR_IMAGE:-ghcr.io/modelcontextprotocol/inspector:latest}

cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

if command -v nerdctl >/dev/null 2>&1; then
  runtime=nerdctl
elif command -v docker >/dev/null 2>&1; then
  runtime=docker
else
  echo "nerdctl or docker is required for CAI_MCP_INSPECTOR_E2E=1" >&2
  exit 1
fi

mkdir -p "$tmpdir"
mkfifo "$fifo"

"$server" --port 0 --print-port >"$fifo" &
server_pid=$!

if ! read -r port <"$fifo"; then
  echo "failed to read MCP test server port" >&2
  exit 1
fi

url="http://127.0.0.1:$port/mcp"

run_inspector() {
  "$runtime" run --rm --network host --entrypoint node "$image" \
    /app/cli/build/index.js "$@"
}

run_inspector "$url" --transport http \
  --method tools/list >"$list_out"

grep '"name": "echo_message"' "$list_out" >/dev/null
grep '"inputSchema"' "$list_out" >/dev/null

run_inspector "$url" --transport http \
  --method tools/call --tool-name echo_message \
  --tool-arg message=inspector-ok >"$call_out"

grep '"structuredContent"' "$call_out" >/dev/null
grep '"echo": "inspector-ok"' "$call_out" >/dev/null
grep '"isError": false' "$call_out" >/dev/null

echo "MCP Inspector e2e passed"
