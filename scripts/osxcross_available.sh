#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
quiet=0
if [[ $# -gt 1 ]]; then
  printf 'usage: %s [--quiet]\n' "$0" >&2
  exit 2
fi

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

description="$("$repo_root/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin)" || { printf 'osxcross: toolchain discovery failed\n' >&2; exit 2; }
if grep -qx 'status=ready' <<<"$description"; then
  if [[ "$quiet" != "1" ]]; then
    printf '%s\n' "$description"
  fi
  exit 0
fi
if ! grep -qx 'status=missing' <<<"$description"; then
  printf 'osxcross: missing or invalid toolchain status\n' >&2
  exit 2
fi

if [[ "$quiet" != "1" ]]; then
  printf '%s\n' "$description" >&2
fi
exit 1
