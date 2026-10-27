#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runner=$cli
peer_start
plan '{"plan":["","",""]}'
"$runner" "$support/lua/goal_continuation.lua" "$endpoint" "$work"
[[ $(requests) == 3 ]]
contains "$root/peer/requests.jsonl" 'Completion audit:'
contains "$root/peer/requests.jsonl" 'Exercise Lua continuation policy'
