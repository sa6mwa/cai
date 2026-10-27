#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
last_request() { tail -1 "$root/peer/requests.jsonl" >"$root/request"; }
ordered_feed() {
  local first second
  first=$(grep -bnFm1 -- "$1" "${3:-$term/feed}" | cut -d: -f1)
  second=$(grep -bnFm1 -- "$2" "${3:-$term/feed}" | cut -d: -f1)
  [[ -n "$first" && -n "$second" && "$first" -lt "$second" ]] || fail "Feed order: $1 before $2"
}
terminal_start -N
# Native steering remains visible/editable until a real runtime seam. No
# runtime admission, prompt echo or provider request may occur before it.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {pause:true,pause_text:"FirstStreamBegin\n\nUnfinished paragraph ",text:"FirstStreamEnd"},"SteerAnswer"
]}')"
send 'Start streaming\r'; expect FirstStreamBegin
send 'STEER_BEFORE_EDIT\033\rQueueFence'; expect_prompt QueueFence
contains "$term/text" 'S 1. STEER_BEFORE_EDIT'
logs
jq -se 'all(.[]; .event != "steering_queued")' "$root/logs.jsonl" >/dev/null
absent "$term/feed" STEER_BEFORE_EDIT
send '\025\033e'; expect_prompt STEER_BEFORE_EDIT
send '\025STEER_AT_SEAM\033\r'; expect 'S 1. STEER_AT_SEAM'
absent "$term/feed" STEER_AT_SEAM
[[ $(requests) == 1 ]]
release; expect SteerAnswer; expect 'Worked for'
ordered_feed FirstStreamEnd '> STEER_AT_SEAM'
ordered_feed '> STEER_AT_SEAM' SteerAnswer
last_request; contains "$root/request" STEER_AT_SEAM
absent "$root/request" STEER_BEFORE_EDIT
# A tool round is another safe seam; the tool result precedes the steer.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {pause:true,pause_text:"ToolStreamBegin\n\n",tool:"get_goal",arguments:{},text:"ToolStreamEnd"},"ToolSteerAnswer"
]}')"
send 'Start a tool round\r'; expect ToolStreamBegin
send 'TOOL_SEAM_STEER\033\r'; expect 'S 1. TOOL_SEAM_STEER'
absent "$term/feed" TOOL_SEAM_STEER
release; expect ToolSteerAnswer; expect 'Worked for'
ordered_feed ToolStreamEnd '> TOOL_SEAM_STEER'
ordered_feed '* Completed get_goal' '> TOOL_SEAM_STEER'
last_request; contains "$root/request" TOOL_SEAM_STEER
# Enter queues a normal turn and does not interrupt the active response.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {pause:true,pause_text:"QueueStreamBegin\n\n",text:"QueueStreamEnd"},"QueuedAnswer"
]}')"
send 'Start another stream\r'; expect QueueStreamBegin
send 'NORMAL_QUEUED_INPUT\r'; expect NORMAL_QUEUED_INPUT
absent "$term/feed" NORMAL_QUEUED_INPUT
release; expect QueuedAnswer; expect 'Worked for'
ordered_feed QueueStreamEnd '> NORMAL_QUEUED_INPUT'
terminal_close
# Native empty Alt-Enter promotes a queued normal entry to steering.
terminal_start
before=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {pause:true,pause_text:"PromotionBegin\n\n",text:"PromotionEnd"},"PromotionDone"
]}')"
send 'Start promotion test\r'; expect PromotionBegin
send 'PROMOTED_INPUT\r'; expect 'Q 1. PROMOTED_INPUT'
send '\033\r'; expect 'S 1. PROMOTED_INPUT'
absent "$term/feed" PROMOTED_INPUT
[[ $(requests) == $((before+1)) ]]
release; expect PromotionDone; expect 'Worked for'
ordered_feed PromotionEnd '> PROMOTED_INPUT'
logs
jq -se 'any(.[]; .event == "steering_delivered" and .msg == "PROMOTED_INPUT")' "$root/logs.jsonl" >/dev/null
terminal_close
# Resume renders input at its consumed marker, not at its earlier acceptance
# record. Check both the selected tail pass and replay-all's direct pass.
for limit in 20 -1; do
  terminal_launch --replay-turns "$limit"
  expect PromotionDone
  ordered_feed FirstStreamEnd '> STEER_AT_SEAM' "$term/text"
  ordered_feed '> STEER_AT_SEAM' SteerAnswer "$term/text"
  ordered_feed QueueStreamEnd '> NORMAL_QUEUED_INPUT' "$term/text"
  ordered_feed PromotionEnd '> PROMOTED_INPUT' "$term/text"
  terminal_close
done

# Cancelling a turn pauses native FIFO release. Its pending turn remains
# editable and runs only after an explicit native idle promotion.
terminal_start -N
before=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {pause:true,pause_text:"CancelQueueBegin\n\n",text:"MustNotComplete"}
]}')"
send 'Start cancellation test\r'; expect CancelQueueBegin
send 'PENDING_AFTER_CANCEL\r'; expect 'Q 1. PENDING_AFTER_CANCEL'
send '\033'; expect 'Worked for'
[[ $(requests) == $((before+1)) ]]
absent "$term/feed" PENDING_AFTER_CANCEL
release
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["PendingPromoted"]}')"
send '\033\r'; expect PendingPromoted; expect 'Worked for'
[[ $(requests) == $((before+2)) ]]
last_request; contains "$root/request" PENDING_AFTER_CANCEL
terminal_close

# A saturated runtime queue leaves additional steering in Softline, rather
# than losing the input or aborting the active stream.
printf '[client]\napi_protocol=%s\n[runtime]\nsteering_queue_limit=1\n' "$protocol" >"$root/protocol.ini"
terminal_start -N
before=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {pause:true,pause_text:"CapacityBegin\n\n",text:"CapacityEnd"},"CapacityNext","CapacityDone"
]}')"
send 'Fill the steering queue\r'; expect CapacityBegin
send 'CAPACITY_ONE\033\r'; expect 'S 1. CAPACITY_ONE'
send 'CAPACITY_TWO\033\r'; expect 'S 2. CAPACITY_TWO'
[[ ! -s "$term/status" && $(requests) == $((before+1)) ]]
absent "$term/feed" CAPACITY_TWO
release; expect CapacityDone; expect 'Worked for'
ordered_feed CapacityEnd '> CAPACITY_ONE'
ordered_feed CapacityNext '> CAPACITY_TWO'
last_request; contains "$root/request" CAPACITY_TWO
[[ $(requests) == $((before+3)) ]]
terminal_close
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"

if [[ "${2:-}" == worker ]]; then
  export CAI_TEST_STEER_WORKER=1
  for enabled in true false; do
    terminal_start -N "$( [[ "$enabled" == true ]] && echo --subagent-steering || echo --no-subagent-steering )"
    before=$(requests)
    if [[ "$enabled" == true ]]; then
      result=WorkerDirected
    else
      result=ParentOriginal
    fi
    plan "$(jq -n --argjson chat "$chat" --arg result "$result" '{chat:$chat,plan:[
      {tool:"run_worker",arguments:{instructions:"Inspect the delegated task"}},
      {pause:true,pause_text:"WorkerStreamBegin\n\n",text:"WorkerStreamEnd"},$result,"ParentDirected"
    ]}')"
    send 'Delegate to the worker\r'; expect WorkerStreamBegin
    send "WORKER_STEER_${enabled}\033\r"; expect "WORKER_STEER_${enabled}"
    absent "$term/feed" "WORKER_STEER_${enabled}"
    expect "S 1. WORKER_STEER_${enabled}"
    release; expect ParentDirected; expect 'Worked for'
    [[ $(requests) == $((before+4)) ]]
    sed -n "$((before+3))p" "$root/peer/requests.jsonl" >"$root/request"
    if [[ "$enabled" == true ]]; then
      contains "$root/request" 'You are the seam test worker.'
      contains "$root/request" WORKER_STEER_true
      ordered_feed WorkerStreamEnd '> WORKER_STEER_true'
    else
      absent "$root/request" WORKER_STEER_false
      last_request; contains "$root/request" WORKER_STEER_false
      ordered_feed ParentOriginal '> WORKER_STEER_false'
    fi
    terminal_close
    if [[ "$enabled" == true ]]; then
      terminal_launch
      expect ParentDirected
      ordered_feed WorkerStreamEnd '> WORKER_STEER_true' "$term/text"
      terminal_close
    fi
  done
  unset CAI_TEST_STEER_WORKER
fi

if [[ "$protocol" == responses ]]; then
  report=$(jq -c . "$support/fixtures/review.json")
  # Manual reviewer: the steer belongs to the child, not a new parent turn.
  terminal_start -N
  before=$(requests)
  plan "$(jq -n --arg report "$report" '{plan:[
    {pause:true,pause_text:"",reasoning:["ChildBarrier"],text:$report},$report
  ]}')"
  send '/review uncommitted\r'; expect ChildBarrier
  send 'REVIEWER_STEER\033\r'; expect 'S 1. REVIEWER_STEER'
  absent "$term/feed" REVIEWER_STEER
  release; expect 'Fix parser'; expect 'Worked for'
  [[ $(requests) == $((before+2)) ]]
  last_request; contains "$root/request" REVIEWER_STEER
  contains "$root/request" ', a code reviewer'
  ordered_feed '> REVIEWER_STEER' '# Review findings'
  terminal_close

  # Model-launched reviewer uses the same foreground-child routing.
  terminal_start -N
  plan "$(jq -n --arg report "$report" '{plan:[
    {tool:"run_review",arguments:{target:"uncommitted"}},
    {pause:true,pause_text:"",reasoning:["ToolChildBarrier"],text:$report},$report,"ParentDone"
  ]}')"
  before=$(requests)
  send 'Delegate the review\r'; expect ToolChildBarrier
  send 'TOOL_REVIEW_STEER\033\r'; expect 'S 1. TOOL_REVIEW_STEER'
  absent "$term/feed" TOOL_REVIEW_STEER
  release; expect ParentDone; expect 'Worked for'
  sed -n "$((before+3))p" "$root/peer/requests.jsonl" >"$root/request"
  contains "$root/request" TOOL_REVIEW_STEER
  contains "$root/request" ', a code reviewer'
  [[ $(requests) == $((before+4)) ]]
  terminal_close

  # Disable through INI, env, or CLI; input stays a parent turn. An explicit
  # enabling flag overrides both inherited settings.
  for setting in ini env flag override; do
    printf '[client]\napi_protocol=responses\n[agent]\nsubagent_steering=%s\n' \
      "$([[ "$setting" == ini || "$setting" == override ]] && echo false || echo true)" >"$root/protocol.ini"
    unset CAI_SUBAGENT_STEERING
    extra=()
    case "$setting" in
      env) export CAI_SUBAGENT_STEERING=false ;;
      flag) extra=(--no-subagent-steering) ;;
      override) export CAI_SUBAGENT_STEERING=false; extra=(--subagent-steering) ;;
    esac
    terminal_start -N "${extra[@]}"
    before=$(requests)
    result=ParentSettingsDone
    if [[ "$setting" == override ]]; then result=$report; fi
    plan "$(jq -n --arg report "$report" --arg result "$result" '{plan:[
      {pause:true,pause_text:"",reasoning:["DisabledChildBarrier"],text:$report},$result
    ]}')"
    send '/review uncommitted\r'; expect DisabledChildBarrier
    send "SETTING_${setting}_INPUT\033\r"; expect "SETTING_${setting}_INPUT"
    absent "$term/feed" "SETTING_${setting}_INPUT"
    input="SETTING_${setting}_INPUT"
    if [[ "$setting" == ini ]]; then
      expect "1. $input"
      wait_for grep -Eq '^> *$' "$term/prompt"
      send '\033e'; expect_prompt "$input"
      input=EDITED_PARENT_INPUT
      send "\025$input\033\r"; expect "$input"
      absent "$term/feed" "$input"
    fi
    expect "1. $input"
    release
    if [[ "$setting" == override ]]; then expect 'Fix parser'; else expect ParentSettingsDone; fi
    expect 'Worked for'
    last_request; contains "$root/request" "$input"
    if [[ "$setting" == ini ]]; then absent "$root/request" SETTING_ini_INPUT; fi
    if [[ "$setting" == override ]]; then
      contains "$root/request" ', a code reviewer'
    else
      absent "$root/request" ', a code reviewer'
      contains "$root/request" '<review_handoff'
    fi
    [[ $(requests) == $((before+2)) ]]
    terminal_close
  done
fi
