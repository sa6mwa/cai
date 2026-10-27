#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runner=$cli
peer_start
printf 'LUA_YOLO_OUTSIDE\n' >"$root/outside"
plan '{"plan":[{"tool":"read_file","arguments":{"path":"../outside"}},"LUA YOLO DONE"]}'
"$runner" "$support/lua/yolo.lua" "$endpoint" "$work"
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" LUA_YOLO_OUTSIDE
