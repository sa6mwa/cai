#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
if [[ "${2:-}" == lua ]]; then
  export CAI_TEST_APP_EXECUTION_PATH="$root/observed.jsonl"
fi
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf 'execution result λ\n' >"$work/input.txt"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"read_file",arguments:{path:42}},
  {tool:"read_file",arguments:{path:"input.txt"}},"EXECUTION DONE"]}')"
run -Nni 'Inspect the input'; ok
[[ $(stdout_text) == 'EXECUTION DONE' ]]
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
run --export "$id"; ok
jsonl=$(head -1 "$root/stdout")
markdown=$(tail -1 "$root/stdout")
jq -s '[.[] | select(.type == "execution") | .data | fromjson]' "$jsonl" >"$root/execution.json"
json "$root/execution.json" 'any(.[]; .kind=="tool.started" and .tool_name=="read_file" and .call_id!=null)'
json "$root/execution.json" 'any(.[]; .kind=="tool.arguments" and (.data|fromjson)=={path:42})'
json "$root/execution.json" 'any(.[]; .kind=="tool.failed" and .code!=0)'
json "$root/execution.json" 'any(.[]; .kind=="tool.failed" and .field=="message" and (.data|length)>0)'
json "$root/execution.json" 'any(.[]; .kind=="tool.output" and (.data|contains("execution result")))'
json "$root/execution.json" 'any(.[]; .kind=="turn.completed")'
jq -L "$support/fixtures" -se 'include "journal"; cai_history | [..|objects|select(.kind?=="tool.failed")] | length==0' "$jsonl" >/dev/null
contains "$markdown" '## Execution history'
contains "$markdown" 'Tool arguments'
contains "$markdown" 'Tool failed'
contains "$markdown" 'execution result'
cp "$jsonl" "$root/original.jsonl"
# Resume without a logfile, replay useful failures, and prove that execution
# records do not leak into the next provider request as extra conversation input.
rm -rf "$root/cache"
terminal_start
expect '* Failed read_file'
before=$(requests)
send '/export\r'; expect 'Conversation export'
expect "$id.md"
[[ $(requests) == "$before" ]]
terminal_close
if [[ "${2:-}" == lua ]]; then
  # Exercise the Lua observer and replay/export boundary once. The remaining
  # import/patch/review/provider-error matrix is shared C code, already covered
  # by both native protocol cases and the dedicated Lua subagent replay test.
  jq -se 'any(.[]; .kind=="tool.failed" and .tool_name=="read_file")' "$root/observed.jsonl" >/dev/null
  exit 0
fi
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["AFTER REPLAY"]}')"
run -ni 'Continue normally'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" '"timestamp_ns"'
absent "$root/request" '"kind": "tool.failed"'
# Import/export preserves the execution timeline without a compatibility shim.
run --import "$root/original.jsonl"; ok
imported=$(awk '{print $NF}' "$root/stdout")
run --list; ok
imported=$(awk 'NR==1 {print $1}' "$root/stdout")
run --export "$imported"; ok
jsonl=$(head -1 "$root/stdout")
jq -s '[.[]|select(.type=="execution")|.data|fromjson]' "$jsonl" >"$root/imported.json"
cmp "$root/execution.json" "$root/imported.json"
if [[ "$protocol" == responses ]]; then
  cat >"$root/patch" <<'PATCH'
*** Begin Patch
*** Add File: execution-patch.txt
+exact "raw" patch λ
*** End Patch
PATCH
  plan "$(jq -n --rawfile patch "$root/patch" '{plan:[{tool:"apply_patch",arguments:$patch},"PATCH DONE"]}')"
  run -Nni 'Apply the patch'; ok
  [[ $(cat "$work/execution-patch.txt") == 'exact "raw" patch λ' ]]
  run --list; ok
  id=$(awk 'NR==1 {print $1}' "$root/stdout")
  run --export "$id"; ok
  jsonl=$(head -1 "$root/stdout")
  jq -sjr '[.[]|select(.type=="execution")|.data|fromjson|select(.kind=="tool.arguments")|.data]|join("")' "$jsonl" >"$root/saved-patch"
  cmp "$root/patch" "$root/saved-patch"

  report=$(jq -c . "$support/fixtures/review.json")
  plan "$(jq -n --arg report "$report" '{plan:[{tool:"read_file",arguments:{path:42}},$report]}')"
  terminal_start -N
  send '/review Inspect the execution fixture\r'; expect 'Fix parser'; expect 'Worked for'
  terminal_close
  run --list; ok
  id=$(awk 'NR==1 {print $1}' "$root/stdout")
  run --export "$id"; ok
  jsonl=$(head -1 "$root/stdout")
  jq -s '[.[]|select(.type=="execution")|.data|fromjson]' "$jsonl" >"$root/review.json"
  jq -e --arg id "$id" 'any(.[]; .kind=="tool.failed" and .tool_name=="read_file" and .subagent=="review" and .parent_call_id=="/review" and .parent_session_id==$id)' "$root/review.json" >/dev/null
  json "$root/review.json" 'any(.[]; .kind=="subagent.input" and (.data|contains("Inspect the execution fixture")))'
  terminal_start
  expect '* Failed read_file'
  terminal_close

  # Bad presentation data must not prevent restoring valid model context.
  jq -c 'if .type=="execution" and (.data|fromjson|.kind=="tool.failed") then .data="{invalid" else . end' "$root/original.jsonl" >"$root/bad.jsonl"
  run --import "$root/bad.jsonl"; ok
  run --list; ok
  id=$(awk 'NR==1 {print $1}' "$root/stdout")
  run --export "$id"; ok
  markdown=$(tail -1 "$root/stdout")
  contains "$markdown" 'could not be decoded'
  before=$(requests)
  terminal_start
  expect 'Historical warning'
  terminal_close
  [[ $(requests) == "$before" ]]

  plan '{"plan":[{"output_items":[{"id":"ws_execution","type":"web_search_call","status":"completed","action":{"type":"search","query":"execution catalog"}}],"text":"HOSTED DONE"}]}'
  run -Nni 'Search'; ok
  run --list; ok
  id=$(awk 'NR==1 {print $1}' "$root/stdout")
  run --export "$id"; ok
  jsonl=$(head -1 "$root/stdout")
  jq -s '[.[]|select(.type=="execution")|.data|fromjson]' "$jsonl" >"$root/hosted.json"
  json "$root/hosted.json" 'any(.[]; .kind=="hosted.tool" and .call_id=="ws_execution" and (.data|fromjson|.action.query)=="execution catalog")'
fi
# Mid-stream provider failure keeps its structured error independently of logs.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {text:"Partial answer",error:"EXECUTION PROVIDER FAILURE",error_code:"execution_failure",error_request_id:"req_execution"}]}')"
run -Nni 'Fail in the provider'; bad
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
run --export "$id"; ok
jsonl=$(head -1 "$root/stdout")
jq -s '[.[]|select(.type=="execution")|.data|fromjson]' "$jsonl" >"$root/failure.json"
json "$root/failure.json" 'any(.[]; .kind=="turn.failed" and (.field=="message" or .field=="detail") and (.data|contains("EXECUTION PROVIDER FAILURE")))'
json "$root/failure.json" 'any(.[]; .kind=="turn.failed" and .field=="provider_code" and .data=="execution_failure")'
json "$root/failure.json" 'any(.[]; .kind=="turn.failed" and .field=="request_id" and .data=="req_execution")'
