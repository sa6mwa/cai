#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
  printf 'usage: %s [repo-root]\n' "$0" >&2
  exit 1
fi

repo_root=${1:-$(pwd)}
version=${CAI_VERSION_OVERRIDE:-}
semver_regex='(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)([.](0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*))?(\+([0-9A-Za-z-]+([.][0-9A-Za-z-]+)*))?'

inside_git=false
if [[ -z "$version" ]] && command -v git >/dev/null 2>&1; then
  if git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git_top=$(git -C "$repo_root" rev-parse --show-toplevel 2>/dev/null || true)
    repo_top=$(cd "$repo_root" && pwd -P)
    if [[ -n "$git_top" && "$(cd "$git_top" && pwd -P)" == "$repo_top" ]]; then
      inside_git=true
      tag=$(git -C "$repo_root" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)
      if [[ -n "$tag" ]]; then
        tag_type=$(git -C "$repo_root" cat-file -t "refs/tags/$tag" 2>/dev/null || true)
        if [[ "$tag_type" != "commit" ]]; then
          printf 'cai release tag must be a lightweight tag: %s resolves to %s\n' \
            "$tag" "${tag_type:-unknown}" >&2
          exit 1
        fi
        if [[ ! "$tag" =~ ^v(${semver_regex})$ ]]; then
          printf 'invalid cai release tag: %s\n' "$tag" >&2
          exit 1
        fi
        version=${tag#v}
      fi
    fi
  fi
fi

if [[ -z "$version" && "$inside_git" != true && -f "$repo_root/VERSION" ]]; then
  version=$(sed 's/^[[:space:]]*//;s/[[:space:]]*$//' "$repo_root/VERSION")
fi

if [[ -z "$version" ]]; then
  version=0.0.0
fi

if [[ ! "$version" =~ ^${semver_regex}$ ]]; then
  printf 'invalid cai release version: %s\n' "$version" >&2
  exit 1
fi

printf '%s\n' "$version"
