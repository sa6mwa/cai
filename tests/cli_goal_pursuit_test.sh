#!/usr/bin/env bash
# Goal pursuit follows durable state, including goals created by model tools.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
mode=${2:-interactive}
scope=${3:-full}
peer_start
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
goal_finished() {
  expect_assistant "$1"
  expect_turn "$send_offset" "$1"
}
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"create_goal",arguments:{objective:"Build the service and verify it"}},
  "Before building, I need to settle the API contract. Proposed defaults:",
  "Implemented the service without another user prompt.",
  {tool:"update_goal",arguments:{status:"complete"}},
  "GOAL COMPLETED"
]}')"
if [[ "$mode" == batch ]]; then
  jq '.plan += ["AFTER THE GOAL"]' "$root/peer/config.json" >"$root/next-plan"
  plan "$(cat "$root/next-plan")"
  run -Nni 'Create a goal and build the service' -i 'After the goal, report'; ok
  [[ $(stdout_text) == 'AFTER THE GOAL' && $(requests) == 6 ]]
else
  terminal_start -N
  send 'Create a goal and build the service\r'
  goal_finished 'GOAL COMPLETED'
  expect 'goal=achieved 0m'
  send '/goal\r'; expect 'Status: complete'
  [[ $(requests) == 5 ]]
  terminal_close
fi
sed -n '3p' "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'Continue pursuing the active goal'
contains "$root/request" 'Build the service and verify it'
[[ "$scope" != basic ]] || exit 0
if [[ "$mode" == batch ]]; then
  count=$(requests)
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {tool:"create_goal",arguments:{objective:"Fail without retrying"}},
    {error:"GOAL PROVIDER FAILED"},"MUST NOT CONTINUE"
  ]}')"
  run -Nni 'Create a goal, then work' -i 'MUST NOT START'; bad
  [[ $(requests) == "$((count+2))" ]]
  contains "$root/stderr" 'GOAL PROVIDER FAILED'
  absent "$root/peer/requests.jsonl" 'MUST NOT START'
  # A blocked goal is a stopped pursuit, not a request for another model turn.
  count=$(requests)
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {tool:"create_goal",arguments:{objective:"Stop when blocked"}},
    {tool:"update_goal",arguments:{status:"blocked"}},
    "BLOCKED FINAL REPORT",
    {error:"MUST NOT CONTINUE"}
  ]}')"
  run -Nni 'Create a goal, then report the blocker' -i 'MUST NOT START'; bad
  [[ $(requests) == "$((count+3))" ]]
  [[ $(stdout_text) == 'BLOCKED FINAL REPORT' ]]
  contains "$root/stderr" 'blocked'
  exit 0
fi

# An approved normal prompt takes priority over an automatic continuation.
count=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"create_goal",arguments:{objective:"Queue ordering"}},
  {pause:true,text:"GOAL FIRST TURN"},"QUEUED TURN",
  {tool:"update_goal",arguments:{status:"complete"}},"QUEUE GOAL DONE"
]}')"
terminal_start -N
send 'Create a goal for queue ordering\r'; expect 'STREAM in flight'
send 'QUEUED USER INPUT\rZ'; expect_prompt Z
send '\025'; release; goal_finished 'QUEUE GOAL DONE'
[[ $(requests) == "$((count+5))" ]]
sed -n "$((count+3))p" "$root/peer/requests.jsonl" >"$root/queued-request"
jq -e '[.. | objects | select(.role? == "user") | .content | if type == "array" then .[] | .text? else . end] | last == "QUEUED USER INPUT"' "$root/queued-request" >/dev/null

# Pause the model-created goal and ensure reopening does not restart it.
count=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"create_goal",arguments:{objective:"Pausable model goal"}},
  {pause:true,text:"PAUSABLE MODEL TURN"}
]}')"
send 'Create a pausable goal\r'; expect 'STREAM in flight'
send '/goal pause\r'; release; expect 'goal=paused'
send '/goal\r'; expect 'Status: paused'
[[ $(requests) == "$((count+2))" ]]
terminal_close
terminal_start
send '/goal\r'; expect 'Pausable model goal'; expect 'Status: paused'
[[ $(requests) == "$((count+2))" ]]
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"update_goal",arguments:{status:"complete"}},"PAUSED GOAL RESUMED"
]}')"
send '/goal resume\r'; goal_finished 'PAUSED GOAL RESUMED'
terminal_close

# Cancelling a model-created active goal must not immediately restart it.
count=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"create_goal",arguments:{objective:"Cancellable model goal"}},
  {pause:true,text:"CANCELLABLE MODEL TURN"}
]}')"
terminal_start -N
send 'Create the cancellable goal\r'; expect 'STREAM in flight'
send '\033'
cancelled_goal() { logs; jq -se 'any(.[]; .event == "run_cancelled")' "$root/logs.jsonl" >/dev/null; }
wait_for cancelled_goal
release
send '/goal\r'; expect 'Status: active'
[[ $(requests) == "$((count+2))" ]]
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"update_goal",arguments:{status:"complete"}},"CANCELLED GOAL CONTINUED"
]}')"
send 'Continue the interrupted goal\r'; goal_finished 'CANCELLED GOAL CONTINUED'
[[ $(requests) == "$((count+4))" ]]
terminal_close

# Failure stops pursuit in this invocation. An active durable goal is pursued
# on restart or explicit /resume, without requiring --goal or /goal resume.
for resume in restart switch; do
  count=$(requests)
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {tool:"create_goal",arguments:{objective:"Recover an active goal"}},
    {error:"GOAL PROVIDER FAILED"}
  ]}')"
  terminal_start -N
  send 'Create the recovery goal\r'; expect 'GOAL PROVIDER FAILED'
  send '/goal\r'; expect 'Status: active'
  [[ $(requests) == "$((count+2))" ]]
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {tool:"update_goal",arguments:{status:"complete"}},"ACTIVE GOAL RECOVERED"
  ]}')"
  if [[ "$resume" == restart ]]; then
    terminal_close
    terminal_start
  else
    send '/new\r'; expect 'New session'
    [[ $(requests) == "$((count+2))" ]]
    send '/resume 2\r'
  fi
  goal_finished 'ACTIVE GOAL RECOVERED'
  [[ $(requests) == "$((count+4))" ]]
  terminal_close
done
