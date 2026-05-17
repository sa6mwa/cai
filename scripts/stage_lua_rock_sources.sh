#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  printf 'usage: %s <repo-root> <stage-dir> <release-version>\n' "$0" >&2
  exit 1
fi

repo_root=$1
stage_dir=$2
release_version=$3

files=(
  LICENSE
  README.md
  cai.rockspec.in
  scripts/build_lua_rock.sh
  scripts/render_release_rockspec.sh
  lua/cai_lua.c
  include/cai/cai.h
  include/cai/mcp.h
  include/cai/models.h
  include/cai/tools/revgeo.h
  include/cai/tools/searxng.h
  include/cai/tools/todo.h
)

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
