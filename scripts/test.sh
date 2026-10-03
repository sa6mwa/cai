#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode=${1:-debug}
if [[ $# -gt 0 ]]; then
  shift
fi
cd "$repo_root"

case "$mode" in
  debug)
    ctest --preset debug "$@"
    ;;
  debug-cai|cai)
    ctest --preset "$mode" "$@"
    ;;
  release|host)
    ctest --preset release "$@"
    ;;
  integration)
    ctest --preset integration "$@"
    ;;
  *)
    printf 'usage: %s [debug|debug-cai|cai|release|host|integration]\n' "$0" >&2
    exit 2
    ;;
esac
