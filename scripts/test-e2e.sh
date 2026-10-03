#!/usr/bin/env bash
set -euo pipefail
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
make -C "$repo_root" build-debug
cmake --build "$repo_root/build/debug" --target cai_mcp_everything_e2e
exec "$repo_root/scripts/run_mcp_everything_service.sh" "$repo_root/build/debug/cai_mcp_everything_e2e" "$("$repo_root/scripts/devenv.sh" endpoint mcp-everything)"
