#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
if [[ "${2:-}" == lua ]]; then
  export CAI_TEST_APP_REPLAY_PATH="$root/lua-replay"
fi
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["PARENT REPLY BEFORE REVIEW"]}')"
run -Nni 'Parent prompt before review'; ok
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
report=$(jq -c '.overall_explanation = (("Review chunk λ. " * 320) + "REVIEW END MARKER")' "$support/fixtures/review.json")
printf 'review input\n' >"$work/input.txt"
plan "$(jq -n --argjson chat "$chat" --arg report "$report" '{chat:$chat,plan:[
  {tool:"read_file",arguments:{path:"input.txt"},preamble:"{REVIEW PROGRESS λ}",reasoning:["Reviewing\n\nInspecting the fixture."]},$report]}')"
terminal_start --resume "$id"
send '/review Inspect this fixture\r'; expect 'Fix parser'; expect 'Worked for'
terminal_close
run --export "$id"; ok
jsonl=$(head -1 "$root/stdout")
jq -s '[.[]|select(.type=="subagent")|.data|fromjson]' "$jsonl" >"$root/children.json"
json "$root/children.json" 'any(.[]; .kind=="assistant_text_delta" and (.data|contains("REVIEW PROGRESS")) and .subagent=="review" and .parent_call_id=="/review" and .source_sequence>0)'
json "$root/children.json" 'any(.[]; .kind=="review_markdown_delta" and (.data|contains("Fix parser")))'
json "$root/children.json" '[.[]|select(.kind=="review_markdown_delta")]|length>1'
json "$root/children.json" 'any(.[]; .kind=="reasoning_summary" and (.data|contains("Inspecting the fixture")))'
jq -e --arg id "$id" 'all(.[]; .session_id!=$id and .parent_session_id==$id)' "$root/children.json" >/dev/null
before=$(requests)
for limit in 20 -1; do
  terminal_start --resume "$id" --replay-turns "$limit"
  expect 'PARENT REPLY BEFORE REVIEW'; expect 'REVIEW PROGRESS'; expect 'Fix parser'
  expect 'REVIEW END MARKER'
  expect 'Review subagent'; expect 'Inspecting the fixture'
  absent "$term/text" 'Tool called:'
  absent "$term/text" 'overall_correctness'
  expect 'Conversation resumed.'
  # The policy notice belongs to startup, before any historical model output.
  first=$(grep -nFm1 'Sandbox restrictions enabled' "$term/text" | cut -d: -f1)
  second=$(grep -nFm1 'PARENT REPLY BEFORE REVIEW' "$term/text" | cut -d: -f1)
  [[ "$first" -lt "$second" ]]
  terminal_close
done
[[ $(requests) == "$before" ]]
logs
jq -se 'any(.[]; .replay==true and .subagent=="review" and .role=="assistant" and (.msg|contains("REVIEW PROGRESS")))' "$root/logs.jsonl" >/dev/null
# Invalid final prose is historical subagent output, never silently lost and
# never promoted to validated findings or normal parent model input.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["[UNVALIDATED REVIEW ANSWER]"]}')"
terminal_start --resume "$id"
send '/review Inspect invalid output\r'; expect 'root value must be an object'
terminal_close
terminal_start --resume "$id"
expect 'PARENT REPLY BEFORE REVIEW'; expect 'UNVALIDATED REVIEW ANSWER'
expect 'root value must be an object'; expect 'Review subagent'
terminal_close
# Cancellation retains the child's partial prose and its cancellation record.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{pause:true,pause_text:"CANCELLED REVIEW PROGRESS\n\n",text:"unused"}]}')"
terminal_start --resume "$id"
send '/review Cancel this review\r'
wait_for assistant_logged $'CANCELLED REVIEW PROGRESS\n\n'
send '\033'; expect 'Review stopped.'; release
terminal_close
terminal_start --resume "$id"
expect 'CANCELLED REVIEW PROGRESS'; expect 'Turn cancelled'
expect 'PARENT REPLY BEFORE REVIEW'; expect 'UNVALIDATED REVIEW ANSWER'
terminal_close
# The parent retains its normal context and committed handoff, but not the
# sideband-only reviewer progress, raw invalid answer or execution envelopes.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["PARENT REPLY AFTER REVIEW"]}')"
run --resume "$id" -ni 'Continue parent'; ok
[[ $(stdout_text) == 'PARENT REPLY AFTER REVIEW' ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'PARENT REPLY BEFORE REVIEW'
absent "$root/request" 'REVIEW PROGRESS'
absent "$root/request" 'UNVALIDATED REVIEW ANSWER'
absent "$root/request" 'source_sequence'
if [[ "${2:-}" == lua ]]; then
  contains "$root/lua-replay" subagent
elif [[ "${2:-}" != binary ]]; then
  # Generic subagent text uses the same parent journal, with no duplicate
  # unattributed assistant-text copy.
  export CAI_TEST_STEER_WORKER=1
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {tool:"run_worker",arguments:{instructions:"Inspect the fixture"}},
    "WORKER REPLAY BODY","PARENT AFTER WORKER"]}')"
  terminal_start -N
  send 'Delegate this task\r'; expect 'PARENT AFTER WORKER'; expect 'Worked for'
  terminal_close
  run --list; ok
  worker_id=$(awk 'NR==1 {print $1}' "$root/stdout")
  terminal_start --resume "$worker_id"
  expect 'WORKER REPLAY BODY'; expect 'PARENT AFTER WORKER'; expect 'Subagent'
  [[ $(grep -c 'WORKER REPLAY BODY' "$term/text") == 1 ]]
  terminal_close
fi
