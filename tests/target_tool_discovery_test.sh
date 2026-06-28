#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

make_tool() {
  local path=$1
  mkdir -p "$(dirname "$path")"
  printf '#!/bin/sh\nexit 0\n' >"$path"
  chmod +x "$path"
}

value_of() {
  local assignments=$1
  local key=$2
  local line
  line=$(printf '%s\n' "$assignments" | grep "^$key=")
  printf '%s\n' "${line#*=}" | xargs printf '%s\n'
}

cache_dir=$tmpdir/cache
tool_dir=$tmpdir/toolchain/bin
mkdir -p "$cache_dir" "$tool_dir"
make_tool "$tool_dir/custom-otool"
make_tool "$tool_dir/arm64-apple-darwin25-clang"
make_tool "$tool_dir/arm64-apple-darwin25-strip"
make_tool "$tool_dir/arm64-apple-darwin25-install_name_tool"
make_tool "$tool_dir/arm64-apple-darwin25-otool"

cat >"$cache_dir/CMakeCache.txt" <<EOF_CACHE
CMAKE_C_COMPILER:FILEPATH=$tool_dir/arm64-apple-darwin25-clang
CPKT_OTOOL:FILEPATH=$tool_dir/custom-otool
EOF_CACHE

assignments=$("$repo_root/scripts/discover_target_tools.sh" \
  "$cache_dir" arm64-apple-darwin)
if [[ "$(value_of "$assignments" OTOOL)" != "$tool_dir/custom-otool" ]]; then
  printf 'expected CPKT_OTOOL cache value, got:\n%s\n' "$assignments" >&2
  exit 1
fi
if [[ "$(value_of "$assignments" STRIP)" != "$tool_dir/arm64-apple-darwin25-strip" ]]; then
  printf 'expected target-prefixed sibling strip, got:\n%s\n' "$assignments" >&2
  exit 1
fi
if [[ "$(value_of "$assignments" INSTALL_NAME_TOOL)" != "$tool_dir/arm64-apple-darwin25-install_name_tool" ]]; then
  printf 'expected target-prefixed sibling install_name_tool, got:\n%s\n' "$assignments" >&2
  exit 1
fi

override_otool=$tmpdir/override/otool
make_tool "$override_otool"
assignments=$(CAI_OTOOL="$override_otool" "$repo_root/scripts/discover_target_tools.sh" \
  "$cache_dir" arm64-apple-darwin)
if [[ "$(value_of "$assignments" OTOOL)" != "$override_otool" ]]; then
  printf 'expected CAI_OTOOL override, got:\n%s\n' "$assignments" >&2
  exit 1
fi

rm -f "$tool_dir/custom-otool"
sed -i '/CPKT_OTOOL/d' "$cache_dir/CMakeCache.txt"
assignments=$("$repo_root/scripts/discover_target_tools.sh" \
  "$cache_dir" arm64-apple-darwin)
if [[ "$(value_of "$assignments" OTOOL)" != "$tool_dir/arm64-apple-darwin25-otool" ]]; then
  printf 'expected target-prefixed sibling otool, got:\n%s\n' "$assignments" >&2
  exit 1
fi

path_dir=$tmpdir/path
make_tool "$path_dir/readelf"
old_path=$PATH
PATH="$path_dir:$PATH"
assignments=$("$repo_root/scripts/discover_target_tools.sh" \
  "$cache_dir" x86_64-linux-gnu)
PATH=$old_path
if [[ "$(value_of "$assignments" READELF)" != "$path_dir/readelf" ]]; then
  printf 'expected PATH readelf fallback, got:\n%s\n' "$assignments" >&2
  exit 1
fi
