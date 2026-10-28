#!/usr/bin/env bash
# Commands use the same native editable queue as conversation prompts.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
command_plan() { plan "$(jq -n --argjson chat "$chat" "{chat:\$chat,plan:$1}")"; }

# Idle commands now pass through the same visible admission path. Edit in the
# native input burst before delivery, including commands that would otherwise
# start a review, change sessions, hand off the terminal or mutate files.
terminal_start -N
before=$(requests)
for command in '/review uncommitted' '/model must-not-select low' \
  '/goal new Must not create' '/new' '/shell' '!touch idle-must-not-exist'; do
  send "$command\r\033e"; expect_prompt "$command"
  absent "$term/feed" "\$ $command"
  send '\025/output compact\r'; expect 'applies from the next tool call'
done
[[ $(requests) == "$before" && ! -e "$work/idle-must-not-exist" ]]
terminal_close

# Escape pauses the old backlog, not newly submitted managed commands. A fresh
# /goal resume must start pursuit without resubmitting or releasing old input.
command_plan '[{pause:true,pause_text:"PolicyGoalBegin\n\n",text:"Cancelled"}]'
terminal_start -N
send '/goal new Native policy resume objective\r'; expect PolicyGoalBegin
send 'STOPPED_ORDINARY_PROMPT\r'; expect 'Q 1. STOPPED_ORDINARY_PROMPT'
send '/goal new STOPPED_GOAL_COMMAND\r'; expect 'Q 2. /goal new STOPPED_GOAL_COMMAND'
send '\033'; expect 'WARN: Turn stopped.'
release
before=$(requests)
send '/status\r'; expect 'Use /tools for the full inventory.'
send '/goal\r'; expect 'Status: paused'
[[ $(requests) == "$before" ]]
command_plan '[{tool:"update_goal",arguments:{status:"complete"}},"PolicyGoalResumed"]'
send '/goal resume\r'; expect PolicyGoalResumed; expect 'goal=achieved'
[[ $(requests) == $((before+2)) ]]
absent "$root/peer/requests.jsonl" STOPPED_ORDINARY_PROMPT
absent "$root/peer/requests.jsonl" STOPPED_GOAL_COMMAND
# Read-only commands and goal pursuit leave old identities editable and stopped.
send '\033e'; expect_prompt '/goal new STOPPED_GOAL_COMMAND'
send '\025/output full\r'; expect 'Q 2. /output full'
absent "$term/feed" '$ /output full'
send '/status\r'; expect 'Use /tools for the full inventory.'
send '\033\r'; expect 'applies from the next tool call'
expect 'Q 1. STOPPED_ORDINARY_PROMPT'
terminal_close
[[ $(requests) == $((before+2)) ]]
