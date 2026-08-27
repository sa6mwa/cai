#!/usr/bin/env bash
set -euo pipefail

mode=${1:-debug}
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${CAI_BUILD_LOCK_HELD:-}" ||
      "${CAI_BUILD_LOCK_HELD}" != "${CAI_BUILD_LOCK_PATH:-}" ]]; then
  exec "$repo_root/scripts/with_build_lock.sh" "$0" "$@"
fi
cd "$repo_root"

case "$mode" in
  debug)
    bash ./scripts/build.sh configure-debug
    ;;
  release|host)
    bash ./scripts/build.sh configure-release
    ;;
  integration)
    bash ./scripts/build.sh configure-integration
    ;;
  *)
    printf 'usage: %s [debug|release|host|integration]\n' "$0" >&2
    exit 2
    ;;
esac
