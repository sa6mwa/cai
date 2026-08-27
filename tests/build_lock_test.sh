#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$(cd "$1" && pwd -P)
lock_helper="$repo_root/scripts/with_build_lock.sh"
tmpdir=$(mktemp -d)
started="$tmpdir/started"
finished="$tmpdir/finished"
lock_path="$tmpdir/build.lock"

cleanup() {
  rm "$started" "$finished" "$lock_path" 2>/dev/null || true
  rmdir "$tmpdir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

CAI_BUILD_LOCK_PATH="$lock_path" "$lock_helper" bash -c '
  : >"$1"
  sleep 1
  : >"$2"
' _ "$started" "$finished" &
holder_pid=$!

for _ in $(seq 1 100); do
  [[ -f "$started" ]] && break
  sleep 0.01
done
if [[ ! -f "$started" ]]; then
  printf '%s\n' 'build lock holder did not start' >&2
  exit 1
fi

CAI_BUILD_LOCK_PATH="$lock_path" "$lock_helper" bash -c '
  if [[ ! -f "$1" ]]; then
    printf "%s\\n" "build lock did not wait for its holder" >&2
    exit 1
  fi
' _ "$finished"
wait "$holder_pid"
