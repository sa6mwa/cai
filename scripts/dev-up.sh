#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$repo_root/devenv/volumes"

"$repo_root/scripts/compose.sh" up -d --build mcp-everything
"$repo_root/scripts/compose.sh" pull searxng
"$repo_root/scripts/compose.sh" up -d searxng

"$repo_root/scripts/compose.sh" ps
printf 'CAI_MCP_EVERYTHING_BASE_URL=%s\n' \
  "${CAI_MCP_EVERYTHING_BASE_URL:-http://127.0.0.1:${CAI_MCP_EVERYTHING_PORT:-3001}/mcp}"
printf 'CAI_SEARXNG_BASE_URL=%s\n' \
  "${CAI_SEARXNG_BASE_URL:-http://127.0.0.1:${CAI_SEARXNG_PORT:-8888}}"
