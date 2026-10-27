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

# Enter must leave goal commands visible and editable until the turn ends.
command_plan '[{pause:true,pause_text:"QueueGoalBegin\n\n",text:"QueueGoalEnd"}]'
terminal_start -N
send '/goal new Queue ownership objective\r'; expect QueueGoalBegin
send '/goal pause\r'; expect 'Q 1. /goal pause'
absent "$term/feed" '$ /goal pause'
logs
jq -se 'all(.[]; .event != "goal_control" or .command != "pause")' "$root/logs.jsonl" >/dev/null
# Queries use the same queue but are eligible between feeds, without consuming
# the unrelated goal control or submitting any model input.
send '/goal\r'; expect 'Queue ownership objective'; expect 'Status: active'
send '/status\033\r'; expect 'Use /tools for the full inventory.'
[[ $(requests) == 1 ]]
# Local queries themselves can be retained in the editor before their feed
# boundary. Both queue insertion and Alt-E arrive in the same native input burst.
send '/goal help\r\033e'; expect_prompt '/goal help'
absent "$term/feed" '$ /goal help'
send '\025/goal status\r'; expect 'Status: active'
send '\033e'; expect_prompt '/goal pause'
send '\025/goal clear\r'; expect 'Q 1. /goal clear'
release; expect 'Goal control queued'
send '/goal\r'; expect 'No goal.'
[[ $(requests) == 1 ]]
absent "$root/peer/requests.jsonl" '/goal clear'
terminal_close

# Alt-Enter also retains commands until a real seam. Edited clear and pause
# commands are local controls, never model steering, and preserve feed order.
before=$(requests)
command_plan '[{tool:"create_goal",arguments:{objective:"Steered goal objective"}},
  {pause:true,pause_text:"SteerGoalBegin\n\n",text:"SteerGoalEnd",tool:"get_goal",arguments:{}},
  "MUST NOT CONTINUE THE PAUSED GOAL"]'
terminal_start -N
send 'Create and work on a goal\r'; expect SteerGoalBegin
send '/goal clear\033\r'; expect 'S 1. /goal clear'
send '\033e'; expect_prompt '/goal clear'
send '\025/goal pause\033\r'; expect 'S 1. /goal pause'
absent "$term/feed" '$ /goal pause'
[[ $(requests) == $((before+2)) ]]
release; expect 'goal=paused'; expect 'Worked for'
contains "$term/feed" '$ /goal pause'
absent "$term/feed" '$ /goal clear'
end_line=$(grep -nF SteerGoalEnd "$term/feed" | cut -d: -f1)
pause_line=$(grep -nF '$ /goal pause' "$term/feed" | cut -d: -f1)
[[ "$end_line" -lt "$pause_line" ]]
[[ $(requests) == $((before+2)) ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" '/goal pause'
absent "$root/request" '/goal status'
terminal_close

# A rejected local steer reports its error once without aborting the turn or
# leaking the command into inference. Quit obeys the same native queue.
command_plan '[{pause:true,pause_text:"InvalidCommandBegin\n\n",text:"InvalidCommandEnd"}]'
terminal_start -N
send 'Stream until the seam\r'; expect InvalidCommandBegin
send '/no-such-command\033\r'; expect 'S 1. /no-such-command'
send '/quit\r'; expect 'Q 2. /quit'
absent "$term/feed" '$ /no-such-command'
absent "$term/feed" '$ /quit'
release
expect 'Unknown command.'
wait_for test -s "$term/status"
wait "$terminal_pid"; terminal_pid=
[[ $(cat "$term/status") == 0 ]]
contains "$term/text" '! Goodbye.'
absent "$term/feed" Goodbye.
absent "$root/peer/requests.jsonl" '/no-such-command'
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD

# Inspecting after Esc must not count as a new user turn or restart stopped
# FIFO delivery. The waiting command remains editable, including while idle.
before=$(requests)
command_plan '[{pause:true,pause_text:"CancelCommandBegin\n\n",text:"CancelledCommandEnd"}]'
terminal_start -N
send 'Wait with a queued command\r'; expect CancelCommandBegin
send '/goal new Must remain queued\r'; expect 'Q 1. /goal new'
send '\033'; expect 'WARN: Turn stopped.'
release
send '/goal\r'; expect 'No goal.'
send '/status\r'; expect 'Use /tools for the full inventory.'
[[ $(requests) == $((before+1)) ]]
send '\033e'; expect_prompt '/goal new Must remain queued'
send '\025/output full\r'; expect 'applies from the next tool call'
[[ $(requests) == $((before+1)) ]]
terminal_close

# Explicitly queued pipe commands retain native editing and FIFO delivery;
# only the edited command may execute.
command_plan '[{pause:true,pause_text:"ShellQueueBegin\n\n",text:"ShellQueueEnd"}]'
terminal_start -N
send 'Wait before running the command\r'; expect ShellQueueBegin
send 'PIPE_QUEUE_PLACEHOLDER\r\033e'; expect_prompt PIPE_QUEUE_PLACEHOLDER
send '\025!printf correct > correct-file\r'; expect 'Q 1. !printf correct'
release; expect 'Shell command exited 0'
[[ $(cat "$work/correct-file") == correct ]]
absent "$root/peer/requests.jsonl" PIPE_QUEUE_PLACEHOLDER
expect ShellQueueEnd; expect 'Worked for'
terminal_close

# The provider has not emitted any text: local inspection must not wait for
# first-token delivery, a tool call, or response completion.
before=$(requests)
command_plan '[{pause:true,pause_text:"",text:"NoFeedDone"}]'
terminal_start -N
send 'Wait without producing any output\r'
no_feed_requested() { [[ $(requests) == $((before+1)) ]]; }
wait_for no_feed_requested
send '/status\r'; expect 'Use /tools for the full inventory.'
send '/goal\r'; expect 'No goal.'
[[ $(requests) == $((before+1)) ]]
absent "$term/feed" NoFeedDone
release; expect_turn "$send_offset" NoFeedDone
terminal_close
