#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
compose_project=${CAI_E2E_COMPOSE_PROJECT:-cai-e2e-$$}
owns_mcp_everything=0

port_is_open() {
  local port=$1
  (: >"/dev/tcp/127.0.0.1/$port") >/dev/null 2>&1
}

if [[ -z "${CAI_MCP_EVERYTHING_PORT:-}" ]]; then
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
if [[ -z "${CAI_MCP_EVERYTHING_PORT:-}" ]]; then
  printf 'Could not find a free local port for MCP Everything e2e.\n' >&2
  exit 1
fi
export CAI_MCP_EVERYTHING_BASE_URL="${CAI_MCP_EVERYTHING_BASE_URL:-http://127.0.0.1:${CAI_MCP_EVERYTHING_PORT}/mcp}"

cleanup() {
  if [[ "$owns_mcp_everything" == "1" && "${CAI_E2E_KEEP_DEVSERVICES:-0}" != "1" ]]; then
    "$repo_root/scripts/compose.sh" -p "$compose_project" stop mcp-everything >/dev/null 2>&1 || true
    "$repo_root/scripts/compose.sh" -p "$compose_project" rm -f mcp-everything >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

existing_mcp_everything=$("$repo_root/scripts/compose.sh" -p "$compose_project" ps -aq mcp-everything)
if [[ -z "$existing_mcp_everything" ]]; then
  owns_mcp_everything=1
fi
"$repo_root/scripts/compose.sh" -p "$compose_project" up -d --build mcp-everything
"$repo_root/scripts/compose.sh" -p "$compose_project" ps mcp-everything
make -C "$repo_root" mcp-everything-wait
make -C "$repo_root" mcp-everything-test
