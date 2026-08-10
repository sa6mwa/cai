#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
test_root=$repo_root/build/release-staging-safety-test
outside_root=$(mktemp -d)

cleanup() {
  rm -rf "$test_root"
  rm -rf "$outside_root"
}
trap cleanup EXIT INT TERM

expect_rejects_without_delete() {
  script=$1
  stage_dir=$2
  shift 2

  mkdir -p "$stage_dir"
  printf '%s\n' keep >"$stage_dir/sentinel"
  if "$repo_root/$script" "$repo_root" "$stage_dir" "$@" >/dev/null 2>&1; then
    printf '%s accepted unsafe stage directory: %s\n' "$script" "$stage_dir" >&2
    exit 1
  fi
  if [ ! -f "$stage_dir/sentinel" ]; then
    printf '%s deleted rejected stage directory: %s\n' "$script" "$stage_dir" >&2
    exit 1
  fi
}

rm -rf "$test_root"
mkdir -p "$test_root/source" "$test_root/lua"

expect_rejects_without_delete \
  scripts/stage_release_sources.sh "$outside_root/source-stage" 1.2.3
expect_rejects_without_delete \
  scripts/stage_lua_rock_sources.sh "$outside_root/lua-stage" 1.2.3

"$repo_root/scripts/stage_release_sources.sh" \
  "$repo_root" "$test_root/source/cai-1.2.3" 1.2.3
if [ ! -f "$test_root/source/cai-1.2.3/RELEASE_MANIFEST" ] ||
   [ ! -f "$test_root/source/cai-1.2.3/VERSION" ]; then
  printf '%s\n' 'stage_release_sources.sh did not create expected generated source stage' >&2
  exit 1
fi

"$repo_root/scripts/stage_lua_rock_sources.sh" \
  "$repo_root" "$test_root/lua/cai-1.2.3" 1.2.3
if [ ! -f "$test_root/lua/cai-1.2.3/RELEASE_MANIFEST" ] ||
   [ ! -f "$test_root/lua/cai-1.2.3/VERSION" ]; then
  printf '%s\n' 'stage_lua_rock_sources.sh did not create expected generated Lua stage' >&2
  exit 1
fi
