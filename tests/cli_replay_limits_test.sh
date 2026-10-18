#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
fixture=${1:?pouch fixture executable}
peer_start
plan '{"plan":["ORIGINAL DURABLE ANSWER"]}'
run -Nni 'original durable prompt'; ok
run --list; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
"$fixture" --oversized-replay-event "$id" "$work"
# Oversized input and shell-result events are omitted, while the surrounding
# conversation replays. Merely resuming must never submit the omitted input.
count=$(requests)
for restart in 1 2; do
  terminal_start
  expect 'Skipped oversized session event during replay'
  expect 'Before omitted terminal transcript'
  expect 'After omitted terminal transcript'
  terminal_close
  [[ $(requests) == "$count" ]]
done
plan '{"plan":["RECOVERED SESSION ANSWER"]}'
run -ni 'continue the recovered session'; ok
contains "$root/stdout" 'RECOVERED SESSION ANSWER'
contains "$root/stderr" 'Skipped oversized session event during replay'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'original durable prompt'
contains "$root/request" 'ORIGINAL DURABLE ANSWER'
absent "$root/request" ZZZZZZZZZZZZZZZZZZZZ
# Skipping the final record must advance the watermark, preserving the raw
# stored records even after new activity is appended. JSONL export is lossless.
run --export "$id"; ok
journal="$root/data/cai/exports/$id/$id.jsonl"
jq -se 'any(.[]; .sequence == 1000001 and .type == "user_shell_completed" and (.data|length) == 131073) and any(.[]; .sequence == 1000003 and .type == "turn_submitted" and (.data|length) == 131073) and any(.[]; .sequence > 1000003 and .type == "turn_submitted" and .data == "continue the recovered session")' "$journal" >/dev/null
plan '{"plan":["RECOVERED REOPEN ANSWER"]}'
run -ni 'continue after reopening'; ok
contains "$root/stdout" 'RECOVERED REOPEN ANSWER'
