#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
  printf 'usage: %s [repo-root]\n' "$0" >&2
  exit 1
fi

repo_root=${1:-$(pwd)}
version=${CAI_VERSION_OVERRIDE:-}

inside_git=false
if [[ -z "$version" ]] && command -v git >/dev/null 2>&1; then
  if git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    inside_git=true
    tag=$(git -C "$repo_root" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)
    if [[ "$tag" =~ ^v([0-9]+\.[0-9]+\.[0-9]+.*)$ ]]; then
      version=${tag#v}
    fi
  fi
fi

if [[ -z "$version" && "$inside_git" != true && -f "$repo_root/VERSION" ]]; then
  version=$(tr -d '[:space:]' <"$repo_root/VERSION")
fi

if [[ -z "$version" ]]; then
  version=0.0.0
fi

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+.*$ ]]; then
  printf 'invalid cai release version: %s\n' "$version" >&2
  exit 1
fi

printf '%s\n' "$version"
