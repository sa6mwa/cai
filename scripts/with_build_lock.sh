#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -eq 0 ]]; then
  printf 'usage: %s <command> [<argument> ...]\n' "$0" >&2
  exit 2
fi

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"

if [[ -n "${CAI_BUILD_LOCK_PATH:-}" ]]; then
  lock_path=$CAI_BUILD_LOCK_PATH
else
  lock_root=${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}
  if [[ ! -d "$lock_root" || ! -w "$lock_root" ]]; then
    lock_root=/tmp
  fi
  read -r lock_key _ < <(printf '%s' "$repo_root" | cksum)
  lock_path="$lock_root/cai-build-${UID:-unknown}-${lock_key}.lock"
fi

case "$lock_path" in
  /*) ;;
  *)
    printf 'with_build_lock.sh: lock path must be absolute: %s\n' "$lock_path" >&2
    exit 2
    ;;
esac

if [[ "${CAI_BUILD_LOCK_HELD:-}" == "$lock_path" ]]; then
  exec "$@"
fi

if ! command -v flock >/dev/null 2>&1; then
  printf '%s\n' 'with_build_lock.sh: flock is required to coordinate generated build state.' >&2
  exit 2
fi

lock_parent=$(dirname -- "$lock_path")
if [[ ! -d "$lock_parent" ]]; then
  mkdir -p -- "$lock_parent"
fi

exec 9>>"$lock_path"
flock -x 9
CAI_BUILD_LOCK_HELD="$lock_path" CAI_BUILD_LOCK_PATH="$lock_path" "$@"
