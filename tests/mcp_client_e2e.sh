#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 /path/to/cai_mcp_http_server /path/to/cai_mcp_client_e2e_driver" >&2
  exit 2
fi

server=$1
client=$2
tmpdir=${TMPDIR:-/tmp}/cai-mcp-client-e2e-$$
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
  echo "failed to read MCP test server port" >&2
  exit 1
fi

"$client" "http://127.0.0.1:$port/mcp"
