#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 /path/to/cai_mcp_http_server /path/to/lua /path/to/e2e_mcp_client.lua" >&2
  exit 2
fi

server=$1
lua_bin=$2
script=$3
tmpdir=${TMPDIR:-/tmp}/cai-lua-mcp-client-e2e-$$
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

stop_server() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=
  fi
}

start_server() {
  rm -f "$fifo"
  mkfifo "$fifo"
  "$server" --port 0 --print-port "$@" >"$fifo" &
  server_pid=$!

  if ! read -r port <"$fifo"; then
    echo "failed to read MCP test server port" >&2
    exit 1
  fi
}

mkdir -p "$tmpdir"

start_server
"$lua_bin" "$script" "http://127.0.0.1:$port/mcp" full
stop_server

start_server --no-tools
"$lua_bin" "$script" "http://127.0.0.1:$port/mcp" empty
stop_server
