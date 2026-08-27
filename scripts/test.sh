#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${CAI_BUILD_LOCK_HELD:-}" ||
      "${CAI_BUILD_LOCK_HELD}" != "${CAI_BUILD_LOCK_PATH:-}" ]]; then
  exec "$repo_root/scripts/with_build_lock.sh" "$0" "$@"
fi
mode=${1:-debug}
if [[ $# -gt 0 ]]; then
  shift
fi
cd "$repo_root"

case "$mode" in
  debug)
    cmake --build --preset debug
    ctest --preset debug "$@"
    ;;
  release|host)
    cmake --build --preset release
    ctest --preset release "$@"
    ;;
  integration)
    cmake --build --preset integration
    ctest --preset integration "$@"
    ;;
  *)
    printf 'usage: %s [debug|release|host|integration]\n' "$0" >&2
    exit 2
    ;;
esac
