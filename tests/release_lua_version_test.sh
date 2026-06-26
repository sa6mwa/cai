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

dry_run=$(CAI_VERSION_OVERRIDE=0.0.0 make -n -C "$repo_root" release-lua-artifacts)
if ! grep -q 'cai-lua-0\.0\.0\.tar\.gz' <<<"$dry_run"; then
  printf '%s\n' 'release-lua-artifacts dry run does not allow fallback source tarball' >&2
  exit 1
fi
if ! grep -q 'cai-0\.0\.0-1\.src\.rock' <<<"$dry_run"; then
  printf '%s\n' 'release-lua-artifacts dry run does not allow fallback src rock' >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

git init -q "$tmpdir/repo"
git -C "$tmpdir/repo" config user.name "cai test"
git -C "$tmpdir/repo" config user.email "cai@example.invalid"
printf 'fixture\n' >"$tmpdir/repo/fixture.txt"
git -C "$tmpdir/repo" add fixture.txt
git -C "$tmpdir/repo" commit -q -m 'fixture'
git -C "$tmpdir/repo" tag v1.2.3
git -C "$tmpdir/repo" worktree add -q "$tmpdir/worktree" HEAD

actual=$(CAI_VERSION_OVERRIDE=$expected "$repo_root/scripts/detect_release_version.sh" "$tmpdir/worktree")
if [[ "$actual" != "$expected" ]]; then
  printf 'detect_release_version returned %s for tagged worktree with override, expected %s\n' "$actual" "$expected" >&2
  exit 1
fi

actual=$("$repo_root/scripts/detect_release_version.sh" "$tmpdir/worktree")
if [[ "$actual" != "1.2.3" ]]; then
  printf 'detect_release_version returned %s for tagged worktree, expected 1.2.3\n' "$actual" >&2
  exit 1
fi

git -C "$tmpdir/repo" worktree add -q "$tmpdir/untagged-worktree" HEAD
git -C "$tmpdir/untagged-worktree" tag -d v1.2.3 >/dev/null
printf '9.9.9\n' >"$tmpdir/untagged-worktree/VERSION"

actual=$("$repo_root/scripts/detect_release_version.sh" "$tmpdir/untagged-worktree")
if [[ "$actual" != "0.0.0" ]]; then
  printf 'detect_release_version returned %s for untagged git worktree with VERSION, expected 0.0.0\n' "$actual" >&2
  exit 1
fi
