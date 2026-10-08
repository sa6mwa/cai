#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

remove_generated_path() {
  local path=$1
  local full_path="$repo_root/$path"

  case "$path" in
    build|dist|.cache) ;;
    *)
      printf 'clean.sh: refusing unexpected generated path: %s\n' "$path" >&2
      exit 1
      ;;
  esac

  case "$full_path" in
    "$repo_root"/*) ;;
    *)
      printf 'clean.sh: refusing path outside repository: %s\n' "$full_path" >&2
      exit 1
      ;;
  esac

  if [[ "$full_path" == "$repo_root" || "$full_path" == / || "$full_path" == "$HOME" ]]; then
    printf 'clean.sh: refusing unsafe path: %s\n' "$full_path" >&2
    exit 1
  fi

  cmake -E rm -rf "$full_path"
}

if [[ $# -gt 1 ]]; then
  printf 'usage: %s [all|dist]\n' "$0" >&2
  exit 2
fi

case "${1:-all}" in
  all)
    cmake "-DROOT=$repo_root" -P "$repo_root/cmake/clean_check.cmake"
    # Keep the generated teardown manifests until their pods are stopped.
    if [[ -d "$repo_root/build/devenv" ]]; then
      "$repo_root/scripts/devenv.sh" down
    fi
    remove_generated_path build
    remove_generated_path dist
    remove_generated_path .cache
    ;;
  dist)
    cmake "-DROOT=$repo_root" -DOWNED=dist -P "$repo_root/cmake/clean_check.cmake"
    remove_generated_path dist
    ;;
  *)
    printf 'usage: %s [all|dist]\n' "$0" >&2
    exit 2
    ;;
esac
