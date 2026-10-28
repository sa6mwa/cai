#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runner=$cli
peer_start
plan '{"plan":["LUA_COMPACT_SOURCE",{"output_items":[{"type":"compaction","id":"cmp_lua","encrypted_content":"LUA_OPAQUE_CONTEXT"}]},"LUA_COMPACT_CONTINUATION","POLL_ONLY_SOURCE",{"output_items":[{"type":"compaction","id":"cmp_poll","encrypted_content":"POLL_OPAQUE_CONTEXT"}]}]}'
"$runner" "$support/lua/compact.lua" "$endpoint" "$work"
[[ $(requests) == 5 ]]
jq -s '.[1]' "$root/peer/requests.jsonl" >"$root/compact"
json "$root/compact" '.input[-1].type == "compaction_trigger" and .previous_response_id == null'
contains "$root/compact" LUA_COMPACT_SOURCE
jq -s '.[2]' "$root/peer/requests.jsonl" >"$root/continuation"
contains "$root/continuation" LUA_OPAQUE_CONTEXT
absent "$root/continuation" LUA_COMPACT_SOURCE
absent "$root/continuation" compaction_trigger
