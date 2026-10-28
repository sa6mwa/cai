#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_VOLATILE_SESSION=1
terminal_start -N
send 'IDLE_CANCEL_DRAFT'; expect_prompt 'IDLE_CANCEL_DRAFT'
send '\033'
idle_editor_cleared() { [[ $(cat "$term/prompt") == '>' ]]; }
wait_for idle_editor_cleared
send '/status\r'; expect 'Metric'
absent "$term/text" 'Turn stopped.'
terminal_close
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
  if [[ "$mode" == cyber_policy || "$mode" == cyber_notice ]]; then
    plan "{\"plan\":[$item,\"Diagnostic explanation for this incident\"]}"
  else
    plan "{\"plan\":[$item]}"
  fi
  send "exercise $mode\r"
  if [[ "$mode" == cancel || "$mode" == escape ]]; then
    expect 'STREAM in flight'
    if [[ "$mode" == cancel ]]; then send '\003'; else send '\033'; fi
    expect 'Worked for'
    expect 'WARN: Turn stopped.'
    release
  fi
  expect 'Worked for'
  absent "$term/text" '[reasoning]'
  if [[ "$mode" == provider_error ]]; then expect 'fixture stream error'; fi
  if [[ "$mode" == cyber_policy || "$mode" == cyber_notice ]]; then
    expect 'Provider rejected the response'
    expect 'Provider cybersecurity notice'
    expect "req_$mode"
    expect 'Diagnostic explanation for this incident'
    logs
    jq -se --arg code "$mode" 'any(.[]; .lvl == "error" and .msg == "cai.response.provider_error" and .provider_code == $code and .request_id == ("req_" + $code) and .detail == "Provider cybersecurity notice")' "$root/logs.jsonl" >/dev/null
  fi
  plan '{"plan":["RECOVERED café λ.\n\n"]}'
  send "recover $mode\r"; expect 'RECOVERED café λ.'
  absent "$term/text" 'STALE cancelled tail.'
  terminal_close
done

# Cancellation during the explanation ends it without a recursive diagnostic.
terminal_start -N
plan '{"plan":[{"error":"Provider incident","error_code":"cyber_policy"},{"pause":true,"text":"CANCELLED DIAGNOSTIC TAIL"}]}'
before=$(requests)
send 'diagnostic cancellation\r'
expect 'STREAM in flight'
send '\003'
expect 'Worked for'
release
[[ $(requests) == $((before+2)) ]]
absent "$term/text" 'CANCELLED DIAGNOSTIC TAIL'
terminal_close

# Policy diagnostics stop automation, while an explicit user turn remains a
# normal agent turn even when a goal exists; restrictions must not leak.
unset CAI_TEST_VOLATILE_SESSION
terminal_start -N
plan '{"plan":[{"error":"Provider incident","error_code":"cyber_policy"},"Goal diagnostic finished"]}'
before=$(requests)
send '/goal new Diagnose interrupted goal\r'
expect 'Goal diagnostic finished'
expect 'Worked for'
[[ $(requests) == $((before+2)) ]]
printf 'MANUAL_READ_MARKER\n' >"$work/tracked.txt"
# Settle the goal inside this turn, before an automatic continuation can start.
# The following restart deliberately tests an idle, paused conversation.
plan '{"plan":["__read_file__",{"tool":"update_goal","arguments":{"status":"paused"}},"Manual followup finished"]}'
send 'Please proceed with my next task\r'
expect_assistant 'Manual followup finished'
expect 'goal=paused'
jq -s '.[-1]' "$root/peer/requests.jsonl" >"$root/manual-request"
contains "$root/manual-request" MANUAL_READ_MARKER
json "$root/manual-request" 'any(.tools[]; .name == "apply_patch")'
terminal_close
before=$(requests)
terminal_start
[[ $(requests) == "$before" ]] || fail 'Resume restarted work automatically'
plan '{"plan":["Manual resumed followup finished"]}'
send 'Another explicit user request\r'
expect_assistant 'Manual resumed followup finished'
terminal_close
logs
jq -se '[.[]|select(.event == "run_cancelled")]|length == 2' "$root/logs.jsonl" >/dev/null
jq -se 'all(.[]|select(.event == "run_cancelled"); .lvl == "warn" and .msg == "Turn stopped.")' "$root/logs.jsonl" >/dev/null

# Headless invocations report provider failures through pslog on stderr and
# exit unsuccessfully even when the automatic diagnostic succeeds.
for code in cyber_policy cyber_notice; do
  plan "{\"plan\":[{\"error\":\"Provider cybersecurity notice\",\"error_code\":\"$code\",\"error_request_id\":\"req_$code\"},\"Diagnostic explanation\"]}"
  before=$(requests)
  run -Nn -i "exercise $code"
  bad
  [[ $(stdout_text) == 'Diagnostic explanation' ]] || fail 'Missing diagnostic final output'
  [[ $(requests) == $((before+2)) ]] || fail 'Unexpected provider request count'
  jq -se --arg code "$code" 'any(.[]; .lvl == "error" and .msg == "cai.response.provider_error" and .provider_code == $code and .request_id == ("req_" + $code) and .detail == "Provider cybersecurity notice")' "$root/stderr" >/dev/null
  absent "$root/stderr" cai.http.transport_error
done
