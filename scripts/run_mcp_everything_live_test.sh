#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 || ! -x "$1" ]]; then
  printf 'usage: %s <executable-cai-integration-test-binary>\n' "$0" >&2
  exit 2
fi
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
export CAI_INTEGRATION_CHATGPT_MCP_CLIENT_TOOL=1
exec "$repo_root/scripts/run_mcp_everything_service.sh" "$1"
