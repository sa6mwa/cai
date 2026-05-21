#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 1
fi

repo_root=$1
expected=7.8.9-test

actual=$(CAI_VERSION_OVERRIDE=$expected "$repo_root/scripts/detect_release_version.sh" "$repo_root")
if [[ "$actual" != "$expected" ]]; then
  printf 'detect_release_version returned %s, expected %s\n' "$actual" "$expected" >&2
  exit 1
fi

actual=$(CAI_VERSION_OVERRIDE=$expected make -s -C "$repo_root" print-release-version)
if [[ "$actual" != "$expected" ]]; then
  printf 'make print-release-version returned %s, expected %s\n' "$actual" "$expected" >&2
  exit 1
fi

dry_run=$(CAI_VERSION_OVERRIDE=$expected make -n -C "$repo_root" release-lua-artifacts)
if grep -q 'cai-lua-0\.0\.0\|cai-0\.0\.0-1' <<<"$dry_run"; then
  printf '%s\n' 'release-lua-artifacts dry run still references 0.0.0' >&2
  exit 1
fi
if ! grep -q "cai-lua-$expected\\.tar\\.gz" <<<"$dry_run"; then
  printf '%s\n' 'release-lua-artifacts dry run does not reference override source tarball' >&2
  exit 1
fi
if ! grep -q "cai-$expected-1\\.src\\.rock" <<<"$dry_run"; then
  printf '%s\n' 'release-lua-artifacts dry run does not reference override src rock' >&2
  exit 1
fi
