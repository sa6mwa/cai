#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <cai-integration-test-binary>\n' "$0" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test_binary=$1
compose_project=${CAI_MCP_EVERYTHING_COMPOSE_PROJECT:-cai-mcp-live-$$}
owns_mcp_everything=0
use_external_mcp_everything=0
tmpdir=$(mktemp -d)

if [[ ! -x "$test_binary" ]]; then
  printf 'MCP Everything live test binary is not executable: %s\n' "$test_binary" >&2
  exit 2
fi

port_is_open() {
  local port=$1
  (: >"/dev/tcp/127.0.0.1/$port") >/dev/null 2>&1
}

if [[ -n "${CAI_MCP_EVERYTHING_BASE_URL:-}" ]]; then
  use_external_mcp_everything=1
elif [[ -n "${CAI_MCP_EVERYTHING_PORT:-}" ]]; then
  if port_is_open "$CAI_MCP_EVERYTHING_PORT"; then
    use_external_mcp_everything=1
  fi
else
  offset=0
  while [[ "$offset" -lt 100 ]]; do
    candidate=$((30000 + (($$ + offset) % 20000)))
    if ! port_is_open "$candidate"; then
      export CAI_MCP_EVERYTHING_PORT=$candidate
      break
    fi
    offset=$((offset + 1))
  done
fi
if [[ -z "${CAI_MCP_EVERYTHING_BASE_URL:-}" &&
  -z "${CAI_MCP_EVERYTHING_PORT:-}" ]]; then
  printf 'Could not find a free local port for MCP Everything live test.\n' >&2
  exit 1
fi
if [[ -z "${CAI_MCP_EVERYTHING_BASE_URL:-}" ]]; then
  export CAI_MCP_EVERYTHING_BASE_URL="http://127.0.0.1:${CAI_MCP_EVERYTHING_PORT}/mcp"
else
  export CAI_MCP_EVERYTHING_BASE_URL
fi

cleanup() {
  if [[ "$owns_mcp_everything" == "1" && "${CAI_E2E_KEEP_DEVSERVICES:-0}" != "1" ]]; then
    "$repo_root/scripts/compose.sh" -p "$compose_project" stop mcp-everything >/dev/null 2>&1 || true
    "$repo_root/scripts/compose.sh" -p "$compose_project" rm -f mcp-everything >/dev/null 2>&1 || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT

if [[ "$use_external_mcp_everything" == "0" ]]; then
  existing_mcp_everything=$("$repo_root/scripts/compose.sh" -p "$compose_project" ps -aq mcp-everything)
  if [[ -z "$existing_mcp_everything" ]]; then
    owns_mcp_everything=1
  fi

  "$repo_root/scripts/compose.sh" -p "$compose_project" up -d --build mcp-everything
  "$repo_root/scripts/compose.sh" -p "$compose_project" ps mcp-everything
fi

init='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cai-live-mcp-wait","version":"0.0.0"}}}'
attempt=1
while [[ "$attempt" -le 60 ]]; do
  if curl -fsS -D "$tmpdir/headers" -o "$tmpdir/body" \
    -H 'content-type: application/json' \
    -H 'accept: application/json, text/event-stream' \
    -d "$init" "$CAI_MCP_EVERYTHING_BASE_URL" >/dev/null 2>&1 &&
    grep -q '"serverInfo"' "$tmpdir/body"; then
    printf 'MCP Everything is ready at %s\n' "$CAI_MCP_EVERYTHING_BASE_URL"
    CAI_INTEGRATION_MCP_CLIENT_TOOL=1 \
      CAI_MCP_EVERYTHING_BASE_URL="$CAI_MCP_EVERYTHING_BASE_URL" \
      "$test_binary"
    exit $?
  fi
  attempt=$((attempt + 1))
  sleep 1
done

printf 'Timed out waiting for MCP Everything at %s\n' "$CAI_MCP_EVERYTHING_BASE_URL" >&2
"$repo_root/scripts/compose.sh" -p "$compose_project" ps mcp-everything >&2 || true
"$repo_root/scripts/compose.sh" -p "$compose_project" logs mcp-everything >&2 || true
exit 1
