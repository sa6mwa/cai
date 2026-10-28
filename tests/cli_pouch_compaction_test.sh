#!/usr/bin/env bash
helper=$(realpath "$2")
source "$(dirname "$0")/cli_test_common.sh"
ulimit -c 0
peer_start
base+=(--disable-compression)
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
# Export initialization has its own current timestamp; stored event timestamps
# and every other byte of semantic state must survive storage compaction.
jq -sc 'map(if .record_type=="commit" then .commit_created_at_ns=0 else . end)' "$root/original.jsonl" >"$root/original-normalized.json"
jq -sc 'map(if .record_type=="commit" then .commit_created_at_ns=0 else . end)' "$root/data/cai/exports/$id/$id.jsonl" >"$root/current-normalized.json"
cmp "$root/current-normalized.json" "$root/original-normalized.json"
cmp "$root/data/cai/exports/$id/$id.md" "$root/original.md"
cmp "$root/state/cai/pouch.key" "$root/original-key"
plan '{"plan":["RESUMED AFTER COMPACTION"]}'
run -ni 'continue after compaction'; ok
contains "$root/stdout" 'RESUMED AFTER COMPACTION'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'durable compaction prompt'
contains "$root/request" 'DURABLE COMPACTION ANSWER'
