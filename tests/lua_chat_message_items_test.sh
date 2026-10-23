#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runner=$cli
peer_start
plan '{"chat":true,"plan":[{"reasoning":["private reasoning"],"deltas":["he","llo \"quoted\" \\ path\n雪"]},"rejected","buffered",{"tool":"echo","arguments":{"x":1},"text":"Checking. "},"done"]}'
"$runner" "$support/lua/chat_message_items.lua" "$root" "$endpoint"
jq -e '.type == "message" and .status == "completed" and .role == "assistant" and .content[0].text == "hello \"quoted\" \\ path\n雪"' "$root/item.json" >/dev/null
jq -se '.[4].messages as $m | [$m[] | select(.tool_calls != null)] | length == 1' "$root/peer/requests.jsonl" >/dev/null
jq -se '.[4].messages as $m | [range(0; $m|length) | select($m[.].tool_calls != null)] as $i | $m[$i[0]+1].role == "tool" and $m[$i[0]-1].role == "assistant" and $m[$i[0]-1].content == "Checking. "' "$root/peer/requests.jsonl" >/dev/null
