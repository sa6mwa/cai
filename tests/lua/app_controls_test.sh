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
export CAI_TEST_APP_LUA_GOAL_RESUME=1
plan '{"plan":[{"pause":true,"text":"Cancel Lua pursuit"},"__complete_goal__","LUA RESUMED GOAL"]}'
run; ok
contains "$root/stdout" 'LUA RESUMED GOAL'
unset CAI_TEST_APP_LUA_GOAL_RESUME
export CAI_TEST_APP_LUA_INTERACTIVE_ERROR=1
plan '{"plan":["Observer failure"]}'
terminal_start -N
send 'initial\r'
wait_for test -s "$term/status"
wait "$terminal_pid" || :
terminal_pid=
[[ $(cat "$term/status") == 1 ]]
absent "$term/text" 'prompt input failed'
