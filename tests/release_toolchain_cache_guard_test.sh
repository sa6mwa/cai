#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 || ! -d "$2" ]]; then
  printf 'usage: %s <repo-root> <existing-build-dir>\n' "$0" >&2
  exit 2
fi

repo_root=$1
build_dir=$2
test_root=$(mktemp -d "$build_dir/toolchain-cache-guard.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/pkg"
cache_root=/var/cache
cpkt_cache_dir_name=c.pkt.systems
cpkt_toolchain_dir_name=toolchains
printf 'cache=%s/%s/%s/roots/bootlin\n' \
  "$cache_root" "$cpkt_cache_dir_name" "$cpkt_toolchain_dir_name" \
  >"$test_root/pkg/libcai.so"

if CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_no_private_bytes '$test_root/pkg'" \
  >/dev/null 2>&1; then
  printf 'expected Bootlin toolchain cache path to fail artifact verification\n' >&2
  exit 1
fi
