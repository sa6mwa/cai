#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
for mode in malformed truncated provider_error cyber_policy cyber_notice cancel escape; do
  terminal_start -N
  case "$mode" in
    malformed) item='{"malformed":true}' ;;
    truncated) item='{"text":"PARTIAL truncated café λ.","truncated":true}' ;;
    provider_error) item='{"text":"PARTIAL provider-error café λ.","error":"fixture stream error"}' ;;
    cyber_policy|cyber_notice)
      item="{\"text\":\"PARTIAL policy response.\",\"error\":\"Provider cybersecurity notice\",\"error_code\":\"$mode\",\"error_event\":\"response.failed\",\"error_request_id\":\"req_$mode\"}"
      ;;
    *) item='{"pause":true,"text":"STALE cancelled tail."}' ;;
  esac
  plan "{\"plan\":[$item]}"
  send "exercise $mode\r"
  if [[ "$mode" == cancel || "$mode" == escape ]]; then
    expect 'STREAM in flight'
    if [[ "$mode" == cancel ]]; then send '\003'; else send '\033'; fi
    expect 'Worked for'
    release
  fi
  expect 'Worked for'
  absent "$term/text" '[reasoning]'
  if [[ "$mode" == provider_error ]]; then expect 'fixture stream error'; fi
  if [[ "$mode" == cyber_policy || "$mode" == cyber_notice ]]; then
    expect 'Provider rejected the response'
    expect 'Provider cybersecurity notice'
    expect "req_$mode"
    logs
    jq -se --arg code "$mode" 'any(.[]; .lvl == "error" and .msg == "cai.response.provider_error" and .provider_code == $code and .request_id == ("req_" + $code) and .detail == "Provider cybersecurity notice")' "$root/logs.jsonl" >/dev/null
  fi
  plan '{"plan":["RECOVERED café λ.\n\n"]}'
  send "recover $mode\r"; expect 'RECOVERED café λ.'
  absent "$term/text" 'STALE cancelled tail.'
  terminal_close
done
logs
jq -se '[.[]|select(.event == "run_cancelled")]|length == 2' "$root/logs.jsonl" >/dev/null

# Headless invocations report provider failures through pslog on stderr and
# exit unsuccessfully; they leave stdout empty if no assistant text arrived.
for code in cyber_policy cyber_notice; do
  plan "{\"plan\":[{\"error\":\"Provider cybersecurity notice\",\"error_code\":\"$code\",\"error_request_id\":\"req_$code\"}]}"
  before=$(requests)
  run -Nn -i "exercise $code"
  bad
  [[ ! -s "$root/stdout" ]] || fail 'Failure without assistant text wrote stdout'
  [[ $(requests) == $((before+1)) ]] || fail 'Unexpected provider request count'
  jq -se --arg code "$code" 'any(.[]; .lvl == "error" and .msg == "cai.response.provider_error" and .provider_code == $code and .request_id == ("req_" + $code) and .detail == "Provider cybersecurity notice")' "$root/stderr" >/dev/null
  absent "$root/stderr" cai.http.transport_error
done
