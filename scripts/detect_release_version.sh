#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
  printf 'usage: %s [repo-root]\n' "$0" >&2
  exit 1
fi

repo_root=${1:-$(pwd)}
version=${CAI_VERSION_OVERRIDE:-}

if [[ -z "$version" && -d "$repo_root/.git" ]] && command -v git >/dev/null 2>&1; then
  tag=$(git -C "$repo_root" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)
  if [[ "$tag" =~ ^v([0-9]+\.[0-9]+\.[0-9]+.*)$ ]]; then
    version=${tag#v}
  fi
fi

if [[ -z "$version" && -f "$repo_root/VERSION" ]]; then
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
