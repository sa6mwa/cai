#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
state_root="$repo_root/devenv/volumes"

case "$state_root" in
  "$repo_root"/devenv/volumes) ;;
  *)
    printf 'Refusing to remove unexpected dev state path: %s\n' "$state_root" >&2
    exit 1
    ;;
esac

"$repo_root/scripts/compose.sh" down -v --remove-orphans
rm -rf "$state_root"
mkdir -p "$state_root"
