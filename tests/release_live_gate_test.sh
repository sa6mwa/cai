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

cleanup() {
  rm -rf "$stamp_dir"
}
trap cleanup EXIT INT TERM

run_gate() {
  (cd "$repo_root" && RELEASE_LIVE_GATE_STAMP=$stamp make --no-print-directory require-prerelease-live)
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

head=$(cd "$repo_root" && git rev-parse HEAD 2>/dev/null || printf unknown)

expect_failure 'missing stamp'

cat >"$stamp" <<EOF
status=failed
head=$head
EOF
expect_failure 'failed stamp status'

cat >"$stamp" <<EOF
status=passed
head=0000000000000000000000000000000000000000
EOF
expect_failure 'stale stamp head'

cat >"$stamp" <<EOF
status=passed
head=$head
target=prerelease-live
timestamp=1970-01-01T00:00:00Z
EOF
expect_success 'current successful stamp'
