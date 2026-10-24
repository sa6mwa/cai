#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
protocol=${1:-responses}
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
metadata '["low","medium","high","xhigh"]'
plan "{\"chat\":$chat,\"plan\":[\"DEFAULT OFF\"]}"
run -Nni first; ok
tail -1 "$root/peer/requests.jsonl" | jq -e 'all(.tools[]?; (.name // .function.name) != "set_reasoning")' >/dev/null
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"RAISED\"]}"
run -Nni complex --reasoning-tool; ok
[[ $(stdout_text) == RAISED ]]; efforts '["medium","high"]'
plan "{\"chat\":$chat,\"plan\":[\"STILL HIGH\"]}"
run -ni continue; ok; efforts '["high"]'
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"normal\"}},\"NORMAL\"]}"
run -ni 'explicitly return to normal' --reasoning-tool; ok
efforts '["high","medium"]'
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"KEEP HIGHER\"]}"
run -Nni complex -r xhigh --reasoning-tool; ok
efforts '["xhigh","xhigh"]'
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
  contains "$root/request" 'changed'
done
metadata '["low","medium","high","xhigh"]'
printf '[agent]\nreasoning_tool = true\nnormal_reasoning_effort = low\nhigh_reasoning_effort = xhigh\n' >>"$root/protocol.ini"
export CAI_NORMAL_REASONING_EFFORT=medium CAI_HIGH_REASONING_EFFORT=high
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"MAPPED\"]}"
run -Nni mapped --high-reasoning-effort xhigh; ok; efforts '["medium","xhigh"]'
unset CAI_NORMAL_REASONING_EFFORT CAI_HIGH_REASONING_EFFORT
terminal_start -N --no-reasoning-tool
send '/reasoning-tool\r'; expect 'off'
send '/reasoning-tool high high\r'; expect 'High effort'
send '/reasoning-tool on\r'; expect 'on'
plan "{\"chat\":$chat,\"plan\":[{\"pause\":true,\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"HOST WINS\"]}"
send 'respect manual control\r'; expect 'STREAM in flight'
send '/reasoning-tool off\r'; expect 'Configure the reasoning tool between turns'
send '/reasoning low\r'; expect 'queued for the next safe'
release; expect_assistant 'HOST WINS'; expect 'Worked for'
efforts '["medium","low"]'
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"set_reasoning\",\"arguments\":{\"mode\":\"high\"}},\"TUI HIGH\"]}"
prompt complex 'TUI HIGH'; efforts '["low","high"]'
send '/reasoning-tool off\r'; expect 'off'
plan "{\"chat\":$chat,\"plan\":[\"TOOL OFF\"]}"
prompt next 'TOOL OFF'
tail -1 "$root/peer/requests.jsonl" | jq -e 'all(.tools[]?; (.name // .function.name) != "set_reasoning")' >/dev/null
terminal_close
