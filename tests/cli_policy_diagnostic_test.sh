#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
mkdir -p "$root/config/cai"
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/config/cai/config.ini"
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf 'LEGITIMATE_SOURCE_MARKER\n' >"$work/relevant.c"
printf 'OUTSIDE_SECRET_MARKER\n' >"$root/outside.c"
for mode in cyber_policy cyber_notice http read list outside bounded disabled tool failure empty ordinary near_match; do
  code=$mode
  status=200
  expected=2
  extra=()
  case "$mode" in http) code=cyber_policy; status=400 ;; read|list|outside|bounded|disabled|tool|failure|empty) code=cyber_policy ;; ordinary) code=server_error ;; near_match) code=cyber_policy_extra ;; esac
  first=$(jq -nc --arg code "$code" --argjson status "$status" '{error:"Provider incident",error_code:$code,error_request_id:"req_incident",http_status:$status}')
  second='"Diagnostic explanation only."'
  case "$mode" in
    tool) second='{"tool":"apply_patch","arguments":{"patch":"*** Begin Patch\n*** Add File: must-not-exist\n+bad\n*** End Patch"}}' ;;
    failure) second='{"error":"Diagnostic also blocked","error_code":"cyber_notice"}' ;;
    empty) second='{"text":""}' ;;
    read) second='{"tool":"read_file","arguments":{"path":"relevant.c"}}' ;;
    list) second='{"tool":"list_files","arguments":{"path":"."}}' ;;
    outside) second='{"tool":"read_file","arguments":{"path":"../outside.c"}}' ;;
    disabled) extra=(--no-read-file --no-list-files) ;;
  esac
  case "$mode" in
    read|list|outside)
      second="$second,\"Diagnostic explanation only.\""
      expected=3 ;;
    bounded)
      second=$(jq -nc '[range(0;9)|{tool:"list_files",arguments:{path:"."}}]|map(tojson)|join(",")' -r)
      expected=10 ;;
  esac
  plan "{\"chat\":$chat,\"plan\":[$first,$second,\"UNEXPECTED CONTINUATION\"]}"
  before=$(requests)
  run -Nn -r low "${extra[@]}" -g 'Summarize legitimate maintenance context' -i 'ORIGINAL_TASK_MARKER' -i 'MUST_NOT_RUN_NEXT'
  bad
  if [[ "$mode" == ordinary || "$mode" == near_match ]]; then
    [[ $(requests) == $((before+1)) ]] || fail 'Non-policy error triggered diagnosis'
    continue
  fi
  [[ $(requests) == $((before+expected)) ]] || fail 'Unexpected diagnostic request count'
  [[ ! -e "$work/must-not-exist" ]] || fail 'Diagnostic executed a tool'
  jq -s '.[-1]' "$root/peer/requests.jsonl" >"$root/diagnostic-request"
  json "$root/diagnostic-request" '.model == "gpt-5.6-luna" and ((.reasoning.effort // .reasoning_effort) == "low")'
  if [[ "$mode" == disabled ]]; then
    json "$root/diagnostic-request" '((.tool_choice // "none") == "none") and ((.tools // []) | length == 0)'
  else
    json "$root/diagnostic-request" '[.tools[]|(.name // .function.name)]|sort == ["list_files","read_file"]'
  fi
  contains "$root/diagnostic-request" 'Produce only an explanation for a policy incident report'
  contains "$root/diagnostic-request" ORIGINAL_TASK_MARKER
  contains "$root/diagnostic-request" 'CAI automatic policy diagnostic'
  contains "$root/diagnostic-request" req_incident
  absent "$root/diagnostic-request" MUST_NOT_RUN_NEXT
  case "$mode" in
    read) contains "$root/diagnostic-request" LEGITIMATE_SOURCE_MARKER ;;
    list) contains "$root/diagnostic-request" relevant.c ;;
    outside) absent "$root/diagnostic-request" OUTSIDE_SECRET_MARKER; contains "$root/diagnostic-request" 'escapes' ;;
  esac
  case "$mode" in
    bounded|tool|failure|empty)
      jq -se 'any(.[]; .msg == "cai.agent.policy_diagnostic" and .status == "failed")' "$root/stderr" >/dev/null ;;
    *) [[ $(stdout_text) == 'Diagnostic explanation only.' ]] || fail 'Diagnostic missing from stdout' ;;
  esac
  jq -se 'any(.[]; .event == "run_failed" and (.msg | contains("cyber_policy") or contains("cyber_notice")))' "$root/stderr" >/dev/null
  run --list; ok
  id=$(awk 'NR==1 {print $1}' "$root/stdout")
  run --export "$id"; ok
  jsonl="$root/data/cai/exports/$id/$id.jsonl"
  markdown="$root/data/cai/exports/$id/$id.md"
  jq -se '.[0].state.goal_status == "active"' "$jsonl" >/dev/null
  if [[ "$mode" == bounded ]]; then contains "$jsonl" 'Source inspection limit reached'; fi
  contains "$markdown" 'CAI automatic policy diagnostic'
  if [[ "$mode" != bounded && "$mode" != tool && "$mode" != failure && "$mode" != empty ]]; then contains "$markdown" 'Diagnostic explanation only.'; fi
done
