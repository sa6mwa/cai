#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'usage: %s <repo-root> <source-tarball>\n' "$0" >&2
  exit 1
fi

repo_root=$1
source_tarball=$2
smoke_root="$repo_root/build/release-source-smoke"
extract_root="$smoke_root/extract"
build_root="$smoke_root/build"

if [[ ! -f "$source_tarball" ]]; then
  printf 'test_release_source.sh: source tarball not found: %s\n' \
    "$source_tarball" >&2
  exit 1
fi

case "$smoke_root" in
  /|"")
    printf 'test_release_source.sh: refusing to use unsafe smoke root\n' >&2
    exit 1
    ;;
esac

rm -rf "$smoke_root"
mkdir -p "$extract_root" "$build_root"

tar -xzf "$source_tarball" -C "$extract_root"

source_root=$(find "$extract_root" -mindepth 1 -maxdepth 1 -type d | sort | head -n 1)
if [[ -z "$source_root" ]]; then
  printf 'test_release_source.sh: failed to locate extracted source tree\n' >&2
  exit 1
fi
if [[ ! -f "$source_root/VERSION" ]]; then
  printf 'test_release_source.sh: extracted source tree is missing VERSION\n' >&2
  exit 1
fi
release_version=$(tr -d '[:space:]' <"$source_root/VERSION")
if [[ ! "$release_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+.*$ ]]; then
  printf 'test_release_source.sh: invalid VERSION value: %s\n' \
    "$release_version" >&2
  exit 1
fi

cmake -S "$source_root" -B "$build_root" -G Ninja -DCAI_BUILD_INTEGRATION_TESTS=OFF
configured_version=$(sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' \
  "$build_root/generated/include/cai/version.h")
if [[ "$configured_version" != "$release_version" ]]; then
  printf 'test_release_source.sh: configured version %s != VERSION %s\n' \
    "$configured_version" "$release_version" >&2
  exit 1
fi
pc_version=$(sed -n 's/^Version: //p' "$build_root/cai.pc")
if [[ "$pc_version" != "$release_version" ]]; then
  printf 'test_release_source.sh: pkg-config version %s != VERSION %s\n' \
    "$pc_version" "$release_version" >&2
  exit 1
fi
cmake --build "$build_root"
ctest --test-dir "$build_root" --output-on-failure --stop-on-failure
