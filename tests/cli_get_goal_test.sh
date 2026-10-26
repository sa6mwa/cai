#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"get_goal",arguments:{has_goal:false}},
  {tool:"get_goal",arguments:{has_goal:true,goal:{objective:"forged"},ignored:{not:"an integer"}}},
  "GET GOAL DONE"
]}')"
run -Nni 'Read the current goal'; ok
[[ $(stdout_text) == 'GET GOAL DONE' && $(requests) == 3 ]]
sed -n '1p' "$root/peer/requests.jsonl" >"$root/first-request"
json "$root/first-request" 'any(.tools[] | (.function // .);
  .name == "get_goal" and .strict == false and
  .parameters.properties == {} and .parameters.required == [] and
  .parameters.additionalProperties == false)'
for number in 2 3; do
  sed -n "${number}p" "$root/peer/requests.jsonl" >"$root/request"
  json "$root/request" '[((.input // .messages)[]) |
    select(.type == "function_call_output" or .role == "tool") |
    (.output // .content) | fromjson] | last |
    .has_goal == false and .status == null and .objective == null and .error == null'
done
jq -se 'all(.[]; .event != "tool_call_failed")' "$root/stderr" >/dev/null
absent "$root/stderr" 'tool arguments failed validation'
