#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  printf 'usage: %s <cai-tests> <test-group> [<test-group> ...]\n' "$0" >&2
  exit 2
fi

binary=$1
shift

if ! command -v valgrind >/dev/null 2>&1; then
  printf 'Valgrind is required for the native memory-check gate. Install valgrind and retry.\n' >&2
  exit 2
fi
if [[ ! -x "$binary" ]]; then
  printf 'Valgrind target is not executable: %s\n' "$binary" >&2
  exit 2
fi

for group in "$@"; do
  valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    --error-exitcode=99 "$binary" --only "$group"
done
