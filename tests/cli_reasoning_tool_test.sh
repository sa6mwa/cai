#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
scenario=${2:?reasoning scenario required: persistence, unsupported, interactive}
case "$scenario" in persistence|unsupported|interactive) ;; *) fail "Unknown reasoning scenario: $scenario" ;; esac
peer_start
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini" --model-metadata "$root/models.json")
metadata() {
  jq -n --argjson efforts "$1" '{models:[{provider:"custom",id:"gpt-5.6-luna",reasoning_efforts:$efforts}]}' >"$root/models.json"
}
efforts() {
  jq -se --argjson chat "$chat" --argjson expected "$1" \
    '.[-($expected|length):] | map(if $chat then .reasoning_effort else .reasoning.effort end) == $expected' "$root/peer/requests.jsonl" >/dev/null
}
acknowledged() {
  tail -1 "$root/peer/requests.jsonl" | jq -e --argjson chat "$chat" '
    (if $chat then [.messages[]? | select(.role == "tool") | .content]
     else [.input[]? | select(.type == "function_call_output") | .output] end)
    | length > 0 and all(.[]; . == "ok")' >/dev/null
  tail -1 "$root/peer/requests.jsonl" >"$root/acknowledged-request"
  absent "$root/acknowledged-request" 'requested high ('
  absent "$root/acknowledged-request" 'requested normal ('
}
receipt() {
  jq -se --arg text "$1" 'any(.[]; .event == "reasoning_tool_result" and .msg == $text)' "$root/stderr" >/dev/null
}
metadata '["low","medium","high","xhigh"]'
if [[ "$scenario" == persistence ]]; then
plan "{\"chat\":$chat,\"plan\":[\"DEFAULT OFF\"]}"
run -Nni first; ok
tail -1 "$root/peer/requests.jsonl" | jq -e 'all(.tools[]?; (.name // .function.name) != "set_reasoning")' >/dev/null
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"RAISED\"]}"
run -Nni complex --reasoning-tool; ok
[[ $(stdout_text) == RAISED ]]; efforts '["medium","high"]'
acknowledged
receipt 'requested high (high): medium -> high'
id=$(current_session_id)
before=$(requests)
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" '* Reasoning requested high (high): medium -> high'
absent "$term/text" 'Accepted reasoning request'
absent "$term/text" 'Completed set_reasoning'
terminal_close
[[ $(requests) == "$before" ]]
plan "{\"chat\":$chat,\"plan\":[\"STILL HIGH\"]}"
run -ni continue; ok; efforts '["high"]'
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"normal\"}},\"NORMAL\"]}"
run -ni 'explicitly return to normal' --reasoning-tool; ok
efforts '["high","medium"]'
acknowledged
receipt 'requested normal (medium): high -> medium'
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"KEEP HIGHER\"]}"
run -Nni complex -r xhigh --reasoning-tool; ok
efforts '["xhigh","xhigh"]'
acknowledged
receipt 'requested high (high): unchanged at xhigh (already at or above requested effort)'
fi
if [[ "$scenario" == unsupported ]]; then
export CAI_TEST_VOLATILE_SESSION=1
for supported in '["low"]' '[]' null; do
  if [[ "$supported" == null ]]; then
    printf '%s\n' '{"models":[{"provider":"custom","id":"gpt-5.6-luna"}]}' >"$root/models.json"
  else metadata "$supported"; fi
  plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"normal\"}},\"CONTINUE TASK\"]}"
  count=$(requests)
  run -Nni 'focus on the task' -r low --reasoning-tool; ok
  [[ $(stdout_text) == 'CONTINUE TASK' && $(requests) == "$((count+4))" ]]
  efforts '["low","low","low","low"]'
  contains "$root/stderr" --high-reasoning-effort
  jq -se '[.[] | select(.event == "cai.reasoning_tool.unavailable")] | length == 2 and all(.lvl == "warn")' "$root/stderr" >/dev/null
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  absent "$root/request" --high-reasoning-effort
  absent "$root/request" 'has no effect'
  acknowledged
  receipt 'requested high (high): unchanged at low (not advertised by model)'
  receipt 'requested normal (medium): unchanged at low (not advertised by model)'
done
fi
if [[ "$scenario" == interactive ]]; then
export CAI_TEST_VOLATILE_SESSION=1
metadata '["low","medium","high","xhigh"]'
printf '[agent]\nreasoning_tool = true\nnormal_reasoning_effort = low\nhigh_reasoning_effort = xhigh\n' >>"$root/protocol.ini"
export CAI_NORMAL_REASONING_EFFORT=medium CAI_HIGH_REASONING_EFFORT=high
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"MAPPED\"]}"
run -Nni mapped --high-reasoning-effort xhigh; ok; efforts '["medium","xhigh"]'
acknowledged
receipt 'requested high (xhigh): medium -> xhigh'
unset CAI_NORMAL_REASONING_EFFORT CAI_HIGH_REASONING_EFFORT
terminal_start -N --no-reasoning-tool
send '/reasoning-tool\r'; expect 'off'
send '/reasoning-tool high high\r'; expect 'High effort'
send '/reasoning-tool on\r'; expect 'on'
plan "{\"chat\":$chat,\"plan\":[{\"pause\":true,\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"HOST WINS\"]}"
send 'respect manual control\r'; expect 'STREAM in flight'
# A whole-turn-only command submitted as a steer is visibly reclassified Q.
# It must not block the next real steer at the upcoming tool boundary.
send '/reasoning-tool off\033\r'; expect 'Q 1. /reasoning-tool off'
send '/reasoning low\033\r'; expect 'S 2. /reasoning low'
release; expect_assistant 'HOST WINS'; expect 'Worked for'
contains "$term/text" '* Reasoning requested high (high):'
absent "$term/feed" 'Configure the reasoning tool between turns'
send '/reasoning-tool\r'; expect 'off'
efforts '["medium","low"]'
acknowledged
send '/reasoning-tool on\r'; expect 'on'
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"TUI HIGH\"]}"
prompt complex 'TUI HIGH'; efforts '["low","high"]'
contains "$term/text" '* Reasoning requested high (high): low -> high'
absent "$term/text" '* Accepted reasoning request'
absent "$term/text" '* Completed set_reasoning'
acknowledged
send '/reasoning-tool off\r'; expect 'off'
plan "{\"chat\":$chat,\"plan\":[\"TOOL OFF\"]}"
prompt next 'TOOL OFF'
tail -1 "$root/peer/requests.jsonl" | jq -e 'all(.tools[]?; (.name // .function.name) != "set_reasoning")' >/dev/null
terminal_close
fi
