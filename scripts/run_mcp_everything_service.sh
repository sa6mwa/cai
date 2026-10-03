#!/usr/bin/env bash
set -euo pipefail
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
devenv="$repo_root/scripts/devenv.sh"
owns_service=0
external=0
if [[ $# -eq 0 ]]; then
  printf 'usage: %s <test-command> [args...]\n' "$0" >&2
  exit 2
fi
if [[ -n "${CAI_MCP_EVERYTHING_BASE_URL:-}" ]]; then
  external=1
fi
cleanup() {
  local rc=$?
  trap - EXIT
  if [[ "$rc" != 0 ]]; then
    printf 'MCP test failed (%s): %s; endpoint=%s\n' "$rc" "$*" "${CAI_MCP_EVERYTHING_BASE_URL:-unavailable}" >&2
    if [[ "$external" == 0 ]]; then
      "$devenv" diagnostics mcp-everything >&2 || true
    fi
  fi
  if [[ "$owns_service" == 1 && "${CAI_E2E_KEEP_DEVSERVICES:-0}" != 1 ]]; then
    if ! "$devenv" down mcp-everything; then
      printf 'Failed to stop the MCP pod owned by this test.\n' >&2
      rc=1
    fi
  fi
  exit "$rc"
}
trap 'cleanup "$@"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ "$external" == 0 ]]; then
  if "$devenv" exists mcp-everything; then
    :
  else
    rc=$?
    [[ "$rc" == 1 ]] || exit "$rc"
    owns_service=1
  fi
  "$devenv" up mcp-everything
  export CAI_MCP_EVERYTHING_BASE_URL
  CAI_MCP_EVERYTHING_BASE_URL=$("$devenv" endpoint mcp-everything)
else
  "$devenv" wait mcp-everything
fi
"$@"
