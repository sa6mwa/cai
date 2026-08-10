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
stub_bin=$stamp_dir/bin
make_log=$stamp_dir/make.log
real_make=$(command -v make)

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

run_prerelease_live() {
  (
    cd "$repo_root"
    CAI_ENABLE_INTEGRATION_TESTS=${CAI_ENABLE_INTEGRATION_TESTS-} \
      CAI_RELEASE_LIVE_MAKE_LOG=${CAI_RELEASE_LIVE_MAKE_LOG:-$make_log} \
      CAI_RELEASE_LIVE_TEST_INTEGRATION_STATUS=${CAI_RELEASE_LIVE_TEST_INTEGRATION_STATUS:-0} \
      GIT_DIR=$git_work_tree/.git \
      GIT_WORK_TREE=$git_work_tree \
      RELEASE_LIVE_GATE_STAMP=$stamp \
      PATH=$stub_bin:$PATH \
      "$real_make" --no-print-directory MAKE=$stub_bin/make prerelease-live
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
mkdir -p "$stub_bin"
git -C "$git_work_tree" init -q
git -C "$git_work_tree" config user.email test@example.invalid
git -C "$git_work_tree" config user.name 'Release Gate Test'
printf '%s\n' test >"$git_work_tree/tracked.txt"
git -C "$git_work_tree" add tracked.txt
git -C "$git_work_tree" commit -q -m 'test fixture'

cat >"$stub_bin/make" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' "$*" >>"$CAI_RELEASE_LIVE_MAKE_LOG"
case "$1" in
  test-integration)
    exit "${CAI_RELEASE_LIVE_TEST_INTEGRATION_STATUS:-0}"
    ;;
esac
EOF
chmod +x "$stub_bin/make"

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

rm -f "$git_work_tree/untracked.txt"

cat >"$stamp" <<EOF
status=passed
head=$head
worktree-status-sha=$clean_status_sha
target=prerelease-live
timestamp=1970-01-01T00:00:00Z
EOF
if CAI_ENABLE_INTEGRATION_TESTS= \
  CAI_RELEASE_LIVE_MAKE_LOG=$make_log \
  run_prerelease_live >"$stdout" 2>"$stderr"; then
  printf '%s\n' 'expected prerelease-live to require explicit live opt-in' >&2
  cat "$stdout" >&2
  cat "$stderr" >&2
  exit 1
fi
if [ -s "$make_log" ]; then
  printf '%s\n' 'prerelease-live ran recursive make without live opt-in' >&2
  cat "$make_log" >&2
  exit 1
fi
expect_success 'existing stamp remains valid after refused opt-in'

: >"$make_log"
cat >"$stamp" <<EOF
status=passed
head=$head
worktree-status-sha=$clean_status_sha
target=prerelease-live
timestamp=1970-01-01T00:00:00Z
EOF
if CAI_ENABLE_INTEGRATION_TESTS=1 \
  CAI_RELEASE_LIVE_TEST_INTEGRATION_STATUS=1 \
  CAI_RELEASE_LIVE_MAKE_LOG=$make_log \
  run_prerelease_live >"$stdout" 2>"$stderr"; then
  printf '%s\n' 'expected prerelease-live to fail when live test fails' >&2
  cat "$stdout" >&2
  cat "$stderr" >&2
  exit 1
fi
if [ -f "$stamp" ]; then
  printf '%s\n' 'prerelease-live left a stale success stamp after failure' >&2
  cat "$stamp" >&2
  exit 1
fi
