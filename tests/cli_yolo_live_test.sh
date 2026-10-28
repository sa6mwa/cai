#!/usr/bin/env bash
# Live policy changes retain native queue ownership and affect later tools only.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
base+=(--terminal)
printf '[client]\napi_protocol=%s\n[agent]\nyolo=false\n' "$protocol" >"$root/policy.ini"
cp "$root/policy.ini" "$root/policy.original"
base+=(--config "$root/policy.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
policy_plan() { plan "$(jq -n --argjson chat "$chat" "{chat:\$chat,plan:$1}")"; }
printf 'LIVE_OUTSIDE\n' >"$root/outside"

policy_plan '[{pause:true,pause_text:"PolicyBegin\n\n",text:"PolicyEnd"}]'
terminal_start -N
send 'Wait before changing policy\r'; expect PolicyBegin
send '/yolo on\r'; expect 'Q 1. /yolo on'
absent "$term/feed" '$ /yolo on'
send '/status\r'; expect 'Pending execution mode'; expect 'Restricted (terminal has no OS sandbox)'
[[ $(requests) == 1 ]]
send '\033e'; expect_prompt '/yolo on'
send '\025/yolo off\r'; expect 'Q 1. /yolo off'
send '\033e'; expect_prompt '/yolo off'
send '\025/yolo on\r'; expect 'Q 1. /yolo on'
release; expect 'YOLO enabled.'
send '/yolo\r'; expect 'on (unrestricted)'
policy_plan '[{tool:"read_file",arguments:{path:"../outside"}},"POLICY READ DONE"]'
prompt 'Read the outside fixture' 'POLICY READ DONE'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" LIVE_OUTSIDE
absent "$root/peer/requests.jsonl" '/yolo on'
send '/yolo off\r'; expect 'YOLO disabled.'
send '/yolo invalid\r'; expect 'Use /yolo [on|off]'
send '/status\r'; expect 'Restricted (terminal has no OS sandbox)'
# Explicit steering is also editable, but cannot replace a policy mid-turn.
policy_plan '[{pause:true,pause_text:"PolicySteerBegin\n\n",text:"PolicySteerEnd"}]'
send 'Wait for the policy steer\r'; expect PolicySteerBegin
send '/yolo on\033\r'; expect 'S 1. /yolo on'
send '\033e'; expect_prompt '/yolo on'
send '\025/yolo on\033\r'; expect 'S 1. /yolo on'
release; expect 'YOLO enabled.'
# Manual reviewers inherit the effective live policy, rather than the INI.
report=$(jq -c . "$support/fixtures/review.json")
policy_plan "$(jq -cn --arg report "$report" '[{pause:true,pause_text:"ReviewPolicyBegin\n\n",tool:"read_file",arguments:{path:"../outside"}},$report]')"
before=$(requests)
send '/review Inspect the fixture\r'
review_requested() { [[ $(requests) == $((before+1)) ]]; }
wait_for review_requested
send '/yolo off\r'; expect 'Q 1. /yolo off'
send '/yolo\r'; expect 'on (unrestricted)'; expect 'Pending:'
release; expect 'Fix parser'; expect 'YOLO disabled.'
tail -1 "$root/peer/requests.jsonl" >"$root/reviewer-request"
contains "$root/reviewer-request" LIVE_OUTSIDE
terminal_close
cmp "$root/policy.ini" "$root/policy.original"

# A managed process can outlive the model turn. The pending policy stays in
# Softline, editable, while its original terminal session is still usable.
policy_plan '[{tool:"exec_command",arguments:{cmd:"read answer; printf got-$answer",yield_time_ms:1}},"LIVE PROCESS READY"]'
terminal_start -N
prompt 'Start a terminal command' 'LIVE PROCESS READY'
send '/yolo on\r'; expect 'Q 1. /yolo on'
send '/yolo\r'; expect 'Pending:'; expect 'off (restricted)'
absent "$term/feed" '$ /yolo on'
send '\033e'; expect_prompt '/yolo on'
send '\025/yolo on\r'; expect 'Q 1. /yolo on'
policy_plan '[{tool:"write_stdin",arguments:{session_id:"terminal-1",chars:"retained\n",yield_time_ms:1000}},"LIVE PROCESS FINISHED"]'
send 'Finish the terminal command\r'; expect 'LIVE PROCESS FINISHED'; expect 'YOLO enabled.'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" got-retained
terminal_close

# Esc pauses native FIFO release; inspections must not restart or bypass it.
policy_plan '[{pause:true,pause_text:"CancelPolicyBegin\n\n",text:"CancelledTail"}]'
terminal_start -N
send 'Wait for cancellation\r'; expect CancelPolicyBegin
send '/yolo on\r'; expect 'Q 1. /yolo on'
send '\033'; expect 'WARN: Turn stopped.'
release
send '/yolo\r'; expect 'off (restricted)'; expect 'Pending:'
absent "$term/feed" '$ /yolo on'
send '\033e'; expect_prompt '/yolo on'
send '\025/yolo\033\r'; expect 'off (restricted)'
terminal_close
cmp "$root/policy.ini" "$root/policy.original"
