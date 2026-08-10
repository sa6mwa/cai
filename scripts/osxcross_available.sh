#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
quiet=0

case "${1:-}" in
  "")
    ;;
  --quiet)
    quiet=1
    ;;
  *)
    printf 'usage: %s [--quiet]\n' "$0" >&2
    exit 2
    ;;
esac

description="$("$repo_root/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin)"
if [[ "$description" == *$'status=ready'* ]]; then
  if [[ "$quiet" != "1" ]]; then
    printf '%s\n' "$description"
  fi
  exit 0
fi

if [[ "$quiet" != "1" ]]; then
  printf '%s\n' "$description" >&2
fi
exit 1
