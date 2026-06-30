#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
stamp_dir=$(mktemp -d)
stamp=$stamp_dir/prerelease-live.stamp
stdout=$stamp_dir/stdout
stderr=$stamp_dir/stderr
git_work_tree=$stamp_dir/worktree

cleanup() {
  rm -rf "$stamp_dir"
}
trap cleanup EXIT INT TERM

run_gate() {
  (
    cd "$repo_root"
    GIT_DIR=$git_work_tree/.git \
      GIT_WORK_TREE=$git_work_tree \
      RELEASE_LIVE_GATE_STAMP=$stamp \
      make --no-print-directory require-prerelease-live
  )
}

expect_failure() {
  label=$1
  if run_gate >"$stdout" 2>"$stderr"; then
    printf 'expected require-prerelease-live to fail: %s\n' "$label" >&2
    cat "$stdout" >&2
    cat "$stderr" >&2
    exit 1
  fi
}

expect_success() {
  label=$1
  if ! run_gate >"$stdout" 2>"$stderr"; then
    printf 'expected require-prerelease-live to pass: %s\n' "$label" >&2
    cat "$stdout" >&2
    cat "$stderr" >&2
    exit 1
  fi
}

mkdir -p "$git_work_tree"
git -C "$git_work_tree" init -q
git -C "$git_work_tree" config user.email test@example.invalid
git -C "$git_work_tree" config user.name 'Release Gate Test'
printf '%s\n' test >"$git_work_tree/tracked.txt"
git -C "$git_work_tree" add tracked.txt
git -C "$git_work_tree" commit -q -m 'test fixture'

head=$(GIT_DIR=$git_work_tree/.git GIT_WORK_TREE=$git_work_tree git rev-parse HEAD)
clean_status_sha=$(
  GIT_DIR=$git_work_tree/.git \
    GIT_WORK_TREE=$git_work_tree \
    git status --porcelain=v1 --untracked-files=all |
    GIT_DIR=$git_work_tree/.git GIT_WORK_TREE=$git_work_tree git hash-object --stdin
)

expect_failure 'missing stamp'

cat >"$stamp" <<EOF
status=failed
head=$head
EOF
expect_failure 'failed stamp status'

cat >"$stamp" <<EOF
status=passed
head=0000000000000000000000000000000000000000
worktree-status-sha=$clean_status_sha
EOF
expect_failure 'stale stamp head'

cat >"$stamp" <<EOF
status=passed
head=$head
worktree-status-sha=$clean_status_sha
target=prerelease-live
timestamp=1970-01-01T00:00:00Z
EOF
expect_success 'current successful stamp'

printf '%s\n' dirty >"$git_work_tree/untracked.txt"
expect_failure 'dirty worktree after stamp'
