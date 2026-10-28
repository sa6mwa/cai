#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
printf '%s\n' '{"models":[{"provider":"custom","id":"gpt-5.6-luna","context_window_tokens":65536,"auto_compact_token_limit":15}]}' >"$root/models.json"
base+=(--model-metadata "$root/models.json")
plan '{"plan":["SEED CONTEXT"]}'
terminal_start -N
prompt 'seed context' 'SEED CONTEXT'
compact='{"type":"compaction","id":"auto","encrypted_content":"AUTO_CONTEXT"}'
for mode in success error cancel; do
  if [[ "$mode" == success ]]; then
    plan "{\"plan\":[{\"pause\":true,\"pause_text\":\"\",\"output_items\":[$compact]},\"AUTO RESUMED\"]}"
  elif [[ "$mode" == error ]]; then
    plan '{"plan":[{"pause":true,"pause_text":"","error":"AUTO COMPACTION ERROR"}]}'
  else
    plan "{\"plan\":[{\"pause\":true,\"pause_text\":\"\",\"output_items\":[$compact]}]}"
  fi
  send "continue $mode\r"
  expect 'Compacting context'
  send '/status\r'; expect 'Use /tools for the full inventory.'
  if [[ "$mode" == cancel ]]; then send '\033'; expect 'Turn stopped.'; fi
  release
  if [[ "$mode" == success ]]; then expect_assistant 'AUTO RESUMED'
  elif [[ "$mode" == error ]]; then expect 'AUTO COMPACTION ERROR'; fi
  wait_for grep -Eq '^! Worked for' "$term/message"
  absent "$term/message" 'Compacting context'
done
terminal_close
before=$(requests)
terminal_start
expect 'Conversation resumed.'
[[ $(requests) == "$before" ]]
terminal_close
logs
jq -se '[.[]|select(.event=="compaction_started")]|length==3' "$root/logs.jsonl" >/dev/null
