#!/usr/bin/env bash
set -euo pipefail
if [[ "${CAI_MCP_INSPECTOR_E2E:-0}" != 1 ]]; then
  echo 'SKIP: set CAI_MCP_INSPECTOR_E2E=1 to run MCP Inspector e2e'
  exit 0
fi
if [[ $# -ne 1 || ! -x "$1" ]]; then
  echo "usage: $0 /path/to/cai_mcp_http_server" >&2
  exit 2
fi
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
devenv="$repo_root/scripts/devenv.sh"
mkdir -p "$repo_root/build/devenv/tmp"
tmpdir=$(mktemp -d "$repo_root/build/devenv/tmp/inspector-test.XXXXXX")
server_pid=
owns_service=0
cleanup() {
  local rc=$?
  trap - EXIT
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$rc" != 0 ]]; then
    cat "$tmpdir"/*.log "$tmpdir"/*.json >&2 2>/dev/null || true
    "$devenv" diagnostics inspector >&2 || true
  fi
  if [[ "$owns_service" == 1 && "${CAI_E2E_KEEP_DEVSERVICES:-0}" != 1 ]]; then
    "$devenv" down inspector || rc=1
  fi
  rm -rf "$tmpdir"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if "$devenv" exists inspector; then
  :
else
  rc=$?
  [[ "$rc" == 1 ]] || exit "$rc"
  owns_service=1
fi
url=$("$devenv" endpoint inspector)
port=${url##*:}
mkfifo "$tmpdir/port.fifo"
exec 3<>"$tmpdir/port.fifo"
"$1" --port "$port" --print-port >"$tmpdir/port.fifo" 2>"$tmpdir/server.log" &
server_pid=$!
if ! read -r -t 10 port <&3; then
  echo 'failed to read local MCP fixture port within 10 seconds' >&2
  exit 1
fi
exec 3>&-
"$devenv" up inspector
url="http://127.0.0.1:$port/mcp"
run_inspector() {
  "$devenv" exec inspector node /app/cli/build/index.js "$@"
}
run_inspector "$url" --transport http --method tools/list >"$tmpdir/tools-list.json"
grep '"name": "echo_message"' "$tmpdir/tools-list.json" >/dev/null
grep '"inputSchema"' "$tmpdir/tools-list.json" >/dev/null
run_inspector "$url" --transport http --method tools/call --tool-name echo_message \
  --tool-arg message=inspector-ok >"$tmpdir/tools-call.json"
grep '"structuredContent"' "$tmpdir/tools-call.json" >/dev/null
grep '"echo": "inspector-ok"' "$tmpdir/tools-call.json" >/dev/null
grep '"isError": false' "$tmpdir/tools-call.json" >/dev/null
echo 'MCP Inspector e2e passed'
