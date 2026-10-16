#!/usr/bin/env bash
helper=$(realpath "$2")
source "$(dirname "$0")/cli_test_common.sh"
ulimit -c 0
peer_start
plan '{"plan":["DURABLE COMPACTION ANSWER"]}'
run -Nni 'durable compaction prompt'; ok
run --list; ok
cp "$root/stdout" "$root/list"
id=$(awk 'NR == 1 {print $1}' "$root/list")
[[ -n "$id" ]]
run --export "$id"; ok
cp "$root/data/cai/exports/$id/$id.jsonl" "$root/original.jsonl"
cp "$root/data/cai/exports/$id/$id.md" "$root/original.md"
cp "$root/state/cai/pouch.key" "$root/original-key"
# Retain real background/default-stack behavior while reducing segment size
# and disabling I/O throttling in this disposable fixture only.
"$helper" "$root/state"
run --list; ok
cmp "$root/stdout" "$root/list"
run --export "$id"; ok
cmp "$root/data/cai/exports/$id/$id.jsonl" "$root/original.jsonl"
cmp "$root/data/cai/exports/$id/$id.md" "$root/original.md"
cmp "$root/state/cai/pouch.key" "$root/original-key"
plan '{"plan":["RESUMED AFTER COMPACTION"]}'
run -ni 'continue after compaction'; ok
contains "$root/stdout" 'RESUMED AFTER COMPACTION'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'durable compaction prompt'
contains "$root/request" 'DURABLE COMPACTION ANSWER'
