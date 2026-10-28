#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
for mode in ready held draft; do
  export CAI_TEST_LATE_STEER=$mode
  terminal_start -N
  before=$(requests)
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {pause:true,pause_text:"IdleSeamBegin\n\n",text:"FirstTurnFinished"},"LateSteerHandled"
  ]}')"
  send 'Start idle seam test\r'; expect IdleSeamBegin
  if [[ "$mode" == draft ]]; then send 'KEEP_THIS_DRAFT'; expect_prompt KEEP_THIS_DRAFT; fi
  release; expect_assistant FirstTurnFinished
  if [[ "$mode" != ready ]]; then
    expect 'LATE_STEER'
    [[ $(requests) == $((before+1)) ]]
    if [[ "$mode" == held ]]; then
      send '\033a\033y'
    else
      expect_prompt KEEP_THIS_DRAFT
      send '\025'
    fi
  fi
  expect_assistant LateSteerHandled
  wait_for has "$term/feed" 'Theme changed to gruvbox:gruvbox.'
  [[ $(requests) == $((before+2)) ]]
  tail -1 "$root/peer/requests.jsonl" >"$root/continuation"
  contains "$root/continuation" LATE_STEER
  terminal_close
done
unset CAI_TEST_LATE_STEER
