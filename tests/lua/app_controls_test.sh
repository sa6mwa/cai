#!/usr/bin/env bash
source "$(dirname "$0")/../cli_test_common.sh"
peer_start
export LOG_LEVEL=warn
plan '{"plan":["Initial reply","Lua queued reply","Resume reply","Observer failure","Stop reply"]}'
run; ok
contains "$root/peer/requests.jsonl" 'LUA NEXT PROMPT'
contains "$root/stderr" 'LUA OBSERVER FAILED'
contains "$root/stderr" 'LUA REPLAY FAILED'
absent "$root/peer/requests.jsonl" 'ABANDON LUA PROMPT'
