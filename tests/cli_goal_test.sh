#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
terminal_start -N
send '/goal\r'; expect 'No goal.'
send '/goal status\r'; expect 'No goal.'
send '/goal help\r'; expect 'Prefer asking the agent to create and manage goals.'
for command in '/goal new' '/goal new   ' '/goal nonsense' '/goal finish this project' '/goal pause extra'; do send "$command\r"; expect 'Use /goal'; done
send '/goal pause\r'; expect 'only an active goal can pause'
send '/goal resume\r'; expect 'goal is not resumable'
[[ $(requests) == 0 ]]
plan '{"plan":[{"text":"FIRST pass","pause":true},{"tool":"update_goal","arguments":{"status":"complete"}},"REPLACEMENT done"]}'
send '/goal new Initial objective\r'; expect 'STREAM in flight'
send '/goal status\r'; expect 'Initial objective'; expect 'unbounded'
send '/goal new Replacement objective\r'
[[ $(requests) == 1 ]]
release; expect_assistant 'REPLACEMENT done'; expect 'Worked for'
[[ $(requests) == 3 ]]
sed -n '2p' "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'Replacement objective'; contains "$root/request" 'Initial objective'
absent "$root/request" '/goal new'
send '/goal status\r'; expect complete
count=$(requests); send '/goal resume\r'; [[ $(requests) == "$count" ]]
plan '{"plan":[{"text":"PAUSE boundary","pause":true}]}'
send '/goal new Pausable objective\r'; expect 'STREAM in flight'
send '/goal pause\r'; release; expect paused
terminal_close
count=$(requests)
terminal_start
send '/goal\r'; expect 'Pausable objective'; expect 'Status: paused'
[[ $(requests) == "$count" ]]
plan '{"plan":[{"tool":"update_goal","arguments":{"status":"complete"}},"RESUMED done"]}'
send '/goal resume\r'; expect_assistant 'RESUMED done'
send '/goal status\r'; expect complete
plan '{"plan":[{"text":"CLEARED partial response","pause":true},"CLEARED turn completed"]}'
send '/goal new Clear objective\r'; expect 'STREAM in flight'
send '/goal clear\r'; release; expect_assistant 'CLEARED turn completed'
send '/goal\r'; expect 'No goal.'
terminal_close
logs
jq -se '[.[]|select(.event == "goal_control")]| all(.lvl == "info" and .role == "user")' "$root/logs.jsonl" >/dev/null
