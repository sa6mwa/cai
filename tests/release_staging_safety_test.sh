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

if ! grep -F 'mktemp -d "$repo_root/build/release-source-stage.XXXXXX"' \
  "$repo_root/scripts/stage_release_sources.sh" >/dev/null; then
  printf '%s\n' 'stage_release_sources.sh must keep manifest scratch under build/' >&2
  exit 1
fi
if find "$repo_root/build" -maxdepth 1 -type d -name 'release-source-stage.*' \
  -print -quit | grep -q .; then
  printf '%s\n' 'stage_release_sources.sh left a release staging workspace behind' >&2
  exit 1
fi

if ! grep -F 'workspace_root="$repo_root/build/release-artifact-verify"' \
  "$repo_root/scripts/verify_release_artifacts.sh" >/dev/null ||
   ! grep -F 'workspace_path=$(mktemp -d "$workspace_root/$label.XXXXXX")' \
    "$repo_root/scripts/verify_release_artifacts.sh" >/dev/null; then
  printf '%s\n' 'release artifact verification must keep extraction and scan workspaces under build/' >&2
  exit 1
fi
if grep -E 'mktemp( -d)?\)' "$repo_root/scripts/verify_release_artifacts.sh" >/dev/null; then
  printf '%s\n' 'release artifact verification must not use an ambient temporary workspace' >&2
  exit 1
fi

"$repo_root/scripts/stage_lua_rock_sources.sh" \
  "$repo_root" "$test_root/lua/cai-1.2.3" 1.2.3
if [ ! -f "$test_root/lua/cai-1.2.3/RELEASE_MANIFEST" ] ||
   [ ! -f "$test_root/lua/cai-1.2.3/VERSION" ]; then
  printf '%s\n' 'stage_lua_rock_sources.sh did not create expected generated Lua stage' >&2
  exit 1
fi
