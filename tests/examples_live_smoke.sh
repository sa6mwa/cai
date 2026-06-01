#!/usr/bin/env bash
set -euo pipefail

repo_root=$1
basic_example=$2
streaming_example=$3
history_example=$4
session_example=$5

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

cd "$repo_root"

"$basic_example" >"$tmpdir/basic.out"
grep -F 'hello from cai' "$tmpdir/basic.out" >/dev/null

"$streaming_example" >"$tmpdir/streaming.out"
test -s "$tmpdir/streaming.out"

"$history_example" >"$tmpdir/history.out"
grep -F '[' "$tmpdir/history.out" >/dev/null
grep -F '"role":"user"' "$tmpdir/history.out" >/dev/null

"$session_example" "$tmpdir/session.json" >"$tmpdir/session.out"
grep -F 'saved:' "$tmpdir/session.out" >/dev/null
grep -F 'restored:' "$tmpdir/session.out" >/dev/null
grep -F '"version"' "$tmpdir/session.json" >/dev/null

