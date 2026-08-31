#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  printf 'usage: %s <repo-root> <stage-dir> <release-version>\n' "$0" >&2
  exit 1
fi

repo_root=$(cd "$1" && pwd -P)
stage_dir=$2
release_version=$3

die() {
  printf 'stage_lua_rock_sources.sh: %s\n' "$1" >&2
  exit 1
}

normalize_stage_dir() {
  local input=$1
  local base parent parent_abs candidate

  if [[ -z "$input" ]]; then
    die 'stage directory is required'
  fi

  case "$input" in
    /*) candidate=$input ;;
    *) candidate="$repo_root/$input" ;;
  esac

  parent=$(dirname "$candidate")
  base=$(basename "$candidate")
  if [[ ! -d "$parent" ]]; then
    die "stage directory parent does not exist: $parent"
  fi
  parent_abs=$(cd "$parent" && pwd -P)
  candidate="$parent_abs/$base"

  case "$candidate" in
    "$repo_root"/build/*|"$repo_root"/dist/*|"$repo_root"/.cache/*) ;;
    *)
      die "refusing stage directory outside generated state: $candidate"
      ;;
  esac
  case "$candidate" in
    "$repo_root"|"$repo_root"/build|"$repo_root"/dist|"$repo_root"/.cache|/|"${HOME:-__no_home__}")
      die "refusing unsafe stage directory: $candidate"
      ;;
  esac

  printf '%s\n' "$candidate"
}

stage_dir=$(normalize_stage_dir "$stage_dir")

files=(
  LICENSE
  README.md
  docs/model-metadata.md
  cai.rockspec.in
  scripts/build_lua_rock.sh
  scripts/render_release_rockspec.sh
  lua/cai_lua.c
)

while IFS= read -r header; do
  files+=("$header")
done < <(cd "$repo_root" && find include/cai -type f -name '*.h' -print | sort)

rm -rf "$stage_dir"
mkdir -p "$stage_dir"

for path in "${files[@]}"; do
  if [[ ! -f "$repo_root/$path" ]]; then
    printf 'missing Lua rock source input: %s\n' "$path" >&2
    exit 1
  fi
  mkdir -p "$stage_dir/$(dirname "$path")"
  cp "$repo_root/$path" "$stage_dir/$path"
done

printf '%s\n' "$release_version" >"$stage_dir/VERSION"
printf '%s\n' "${files[@]}" VERSION >"$stage_dir/RELEASE_MANIFEST"
