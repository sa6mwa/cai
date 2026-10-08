#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
for mode in malformed truncated provider_error cancel escape; do
  terminal_start -N
  case "$mode" in
    malformed) item='{"malformed":true}' ;;
    truncated) item='{"text":"PARTIAL truncated café λ.","truncated":true}' ;;
    provider_error) item='{"text":"PARTIAL provider-error café λ.","error":"fixture stream error"}' ;;
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
  plan '{"plan":["RECOVERED café λ.\n\n"]}'
  send "recover $mode\r"; expect 'RECOVERED café λ.'
  absent "$term/text" 'STALE cancelled tail.'
  terminal_close
done
logs
jq -se '[.[]|select(.event == "run_cancelled")]|length == 2' "$root/logs.jsonl" >/dev/null
