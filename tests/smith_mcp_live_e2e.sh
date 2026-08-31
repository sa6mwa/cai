#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
  printf 'usage: %s /path/to/cai_mcp_http_server command [args...]\n' "$0" >&2
  exit 2
fi

server=$1
shift
tmpdir=${TMPDIR:-/tmp}/cai-smith-mcp-live-e2e-$$
fifo=$tmpdir/port.fifo
server_pid=

cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

mkdir -p "$tmpdir"
mkfifo "$fifo"
"$server" --port 0 --print-port >"$fifo" &
server_pid=$!

if ! read -r port <"$fifo"; then
  printf '%s\n' 'failed to read Smith MCP e2e server port' >&2
  exit 1
fi

CAI_SMITH_E2E_MCP_URL="http://127.0.0.1:$port/mcp"
export CAI_SMITH_E2E_MCP_URL
"$@"
