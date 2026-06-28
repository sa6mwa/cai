#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
started=0

if [[ -z "${CAI_MCP_EVERYTHING_PORT:-}" ]]; then
  for offset in $(seq 0 99); do
    candidate=$((30000 + (($$ + offset) % 20000)))
    if ! timeout 1 bash -c ":</dev/tcp/127.0.0.1/$candidate" \
      >/dev/null 2>&1; then
      export CAI_MCP_EVERYTHING_PORT=$candidate
      break
    fi
  done
fi
if [[ -z "${CAI_MCP_EVERYTHING_PORT:-}" ]]; then
  printf 'Could not find a free local port for MCP Everything e2e.\n' >&2
  exit 1
fi
export CAI_MCP_EVERYTHING_BASE_URL="${CAI_MCP_EVERYTHING_BASE_URL:-http://127.0.0.1:${CAI_MCP_EVERYTHING_PORT}/mcp}"

cleanup() {
  if [[ "$started" == "1" && "${CAI_E2E_KEEP_DEVSERVICES:-0}" != "1" ]]; then
    "$repo_root/scripts/compose.sh" stop mcp-everything >/dev/null 2>&1 || true
    "$repo_root/scripts/compose.sh" rm -f mcp-everything >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

"$repo_root/scripts/compose.sh" up -d --build mcp-everything
started=1
"$repo_root/scripts/compose.sh" ps mcp-everything
make -C "$repo_root" mcp-everything-wait
make -C "$repo_root" mcp-everything-test
