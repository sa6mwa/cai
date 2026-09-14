#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
test_root=$(mktemp -d "$repo_root/build/toolchain-cache-guard.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/pkg"
printf 'cache=/var/cache/c.pkt.systems/toolchains/roots/bootlin\n' \
  >"$test_root/pkg/libcai.so"

if CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_no_private_bytes '$test_root/pkg'" \
  >/dev/null 2>&1; then
  printf 'expected Bootlin toolchain cache path to fail artifact verification\n' >&2
  exit 1
fi
