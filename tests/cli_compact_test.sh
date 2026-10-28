#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf '[client]\napi_protocol=%s\n[agent]\ndeveloper_instructions=COMPACT_POLICY\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
mkdir -p "$root/config/cai"
printf 'COMPACT_GLOBAL_INSTRUCTIONS\n' >"$root/config/cai/AGENTS.md"
compact='{"output_items":[{"type":"compaction","id":"cmp_manual","encrypted_content":"MANUAL_OPAQUE_CONTEXT"}]}'
[[ "$chat" == false ]] || compact='{"text":"MANUAL_CHAT_SUMMARY"}'
set_plan() { plan "$(jq -nc --argjson chat "$chat" --argjson p "$1" '{chat:$chat,plan:$p}')"; }
history_state() {
  run --export "$id"; ok
  jq -sc '[.[] | select(.record_type == "checkpoint")][-1].state.history' \
    "$root/data/cai/exports/$id/$id.jsonl" >"$1"
}

terminal_start -N
id=$(current_session_id)
send '/compact extra\r'; expect 'Usage: /compact'
send '/compact\r'; expect 'history'; expect 'Worked for'
[[ $(requests) == 0 ]]

# Compaction is an editable normal queue entry, even with Alt-Enter. It must
# not interrupt streamed text, become a steer, or add a synthetic user prompt.
set_plan "[{\"pause\":true,\"text\":\"ToolSeamBeforeCompact\",\"tool\":\"get_goal\",\"arguments\":{}},\"BeforeCompactAnswer\",$compact]"
send 'COMPACT_SOURCE_PROMPT\r'; expect 'STREAM in flight'
send '/compact\033\r'; expect 'Q 1. /compact'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/queued-feed"
absent "$root/queued-feed" '$ /compact'
send '\033e'; expect_prompt '/compact'
send '\025/compact\r'; expect 'Q 1. /compact'
[[ $(requests) == 1 ]]
# Empty Alt-Enter promotes the newest native queue entry without going
# through the draft hook. It must still wait past the upcoming tool seam.
send '\033\r'
release; expect BeforeCompactAnswer; expect 'Context compacted.'
[[ $(requests) == 3 ]]
logs
jq -se 'any(.[]; .event == "run_started" and .role == "runtime" and .prompt_kind == "compaction")' "$root/logs.jsonl" >/dev/null
tail -1 "$root/peer/requests.jsonl" >"$root/compact-request"
contains "$root/compact-request" COMPACT_SOURCE_PROMPT
if [[ "$chat" == false ]]; then
  json "$root/compact-request" '.input[-1].type == "compaction_trigger" and (.previous_response_id == null) and (.conversation == null)'
  contains "$root/compact-request" COMPACT_GLOBAL_INSTRUCTIONS
  contains "$root/compact-request" COMPACT_POLICY
else
  contains "$root/compact-request" 'Transcript fragment:'
fi
send '/trim\r'; expect 'Compacted'
terminal_close

if [[ "$chat" == false ]]; then
  # A review owns a separate worker: compaction waits in the parent's queue
  # instead of steering the reviewer or starting on its isolated runtime.
  before=$(requests)
  report=$(jq -c . "$support/fixtures/review.json")
  set_plan "$(jq -nc --arg report "$report" --argjson compact "$compact" \
    '[{pause:true,pause_text:"",text:$report},$compact]')"
  terminal_start --resume "$id"
  send '/review Inspect the fixture\r'
  review_requested() { [[ $(requests) == $((before+1)) ]]; }
  wait_for review_requested
  send '/compact\033\r'; expect 'Q 1. /compact'
  [[ $(requests) == $((before+1)) ]]
  release; expect 'Context compacted.'
  [[ $(requests) == $((before+2)) ]]
  terminal_close

  # An idle model with a running user command is not a compaction boundary.
  before=$(requests)
  mkfifo "$work/compact.gate"
  set_plan "[$compact]"
  terminal_start --resume "$id"
  send '!cat compact.gate\r'; expect '$ !cat compact.gate'
  send '/compact\r'; expect 'Q 1. /compact'
  [[ $(requests) == "$before" ]]
  printf 'ShellGateFinished\n' >"$work/compact.gate"
  expect 'Context compacted.'
  [[ $(requests) == $((before+1)) ]]
  terminal_close
fi

# The opaque replacement and fresh instructions survive close/reopen, and
# manual compaction never requests a normal answer after its own completion.
set_plan '["AfterManualCompaction"]'
terminal_start --resume "$id"
prompt 'CONTINUE_AFTER_MANUAL' AfterManualCompaction
tail -1 "$root/peer/requests.jsonl" >"$root/continued"
if [[ "$chat" == false ]]; then
  contains "$root/continued" MANUAL_OPAQUE_CONTEXT
  absent "$root/continued" compaction_trigger
  contains "$root/continued" COMPACT_GLOBAL_INSTRUCTIONS
  contains "$root/continued" COMPACT_POLICY
else
  contains "$root/continued" MANUAL_CHAT_SUMMARY
fi
absent "$root/continued" BeforeCompactAnswer
absent "$root/continued" /compact
history_state "$root/before.json"

# A provider rejection leaves the exact history intact and the app usable.
for code in invalid_request_error cyber_notice cyber_policy; do
  before=$(requests)
  set_plan "[{\"http_status\":400,\"error_code\":\"$code\",\"error\":\"MANUAL_COMPACT_REJECTED_$code\"}]"
  send '/compact\r'; expect "MANUAL_COMPACT_REJECTED_$code"; expect 'Worked for'
  history_state "$root/after.json"; cmp "$root/before.json" "$root/after.json"
  [[ $(requests) == $((before+1)) ]]
done

if [[ "$chat" == false ]]; then
  # An ordinary answer is not a valid compaction response. Neither it nor
  # tool calls returned by the compactor may enter history or execute.
  set_plan '[{"tool":"apply_patch","arguments":"*** Begin Patch\n*** Add File: forbidden\n+bad\n*** End Patch","text":"INVALID_COMPACTION_TEXT"}]'
  send '/compact\r'; expect 'compaction stream did not complete'; expect 'Worked for'
  [[ ! -e "$work/forbidden" ]]
  history_state "$root/after.json"; cmp "$root/before.json" "$root/after.json"
  for bad in \
    '{"type":"compaction","id":"cmp_bad"}' \
    '{"type":"compaction","id":"cmp_bad","encrypted_content":42}' \
    '{"type":"compaction","id":"cmp_bad","encrypted_content":""}'; do
    set_plan "[{\"output_items\":[$bad]}]"
    send '/compact\r'; expect 'invalid compaction payload'; expect 'Worked for'
    history_state "$root/after.json"; cmp "$root/before.json" "$root/after.json"
  done
  set_plan '[{"output_items":[{"type":"compaction","encrypted_content":"one"},{"type":"compaction","encrypted_content":"two"}]}]'
  send '/compact\r'; expect 'exactly one item'; expect 'Worked for'
  history_state "$root/after.json"; cmp "$root/before.json" "$root/after.json"
fi

# A blocked compaction does not block the UI. Escape cancels the request and
# leaves context resumable, rather than retrying or silently losing history.
before=$(requests)
set_plan '[{"pause":true,"pause_text":"","text":"CANCELLED_SUMMARY"}]'
send '/compact\r'; expect 'Compacting context'
requested() { [[ $(requests) == $((before+1)) ]]; }
wait_for requested
send '/status\r'; expect 'Use /tools for the full inventory.'
send '\033'; expect 'WARN: Turn stopped.'
release
history_state "$root/after.json"; cmp "$root/before.json" "$root/after.json"
terminal_close
set_plan '["AfterCancelledCompaction"]'
terminal_start --resume "$id"
prompt 'CONTINUE_AFTER_CANCEL' AfterCancelledCompaction
tail -1 "$root/peer/requests.jsonl" >"$root/continued"
absent "$root/continued" CANCELLED_SUMMARY
terminal_close
