#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
history="$root/state/cai/history/$(printf %s "$work" | sha256sum | cut -d' ' -f1).history"
finish_alt_q() {
  send '\033q'
  wait_for test -s "$term/status"
  wait "$terminal_pid" || fail 'Alt-Q application failed'
  terminal_pid=
  [[ $(cat "$term/status") == 0 ]] || fail 'Alt-Q returned nonzero'
  [[ ! -s "$term/stderr" ]] || fail 'Unexpected terminal diagnostics'
  contains "$term/text" Goodbye.
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD
}
terminal_start -N
send '\033[200~draft å€\nsecond tail\033[201~'
finish_alt_q
printf 'draft å€\\nsecond tail\n' >"$root/expected"
cmp "$history" "$root/expected"
[[ $(requests) == 0 && $(stat -c %a "$history") == 600 ]]
# Up recalls without submitting, and saving the same last entry is a no-op.
terminal_start
send '\033[A'; expect_prompt 'second tail'
finish_alt_q
cmp "$history" "$root/expected"
[[ $(requests) == 0 ]]
# Verify the exact recalled multiline Unicode text only when Enter is pressed.
terminal_start
send '\033[A'; expect_prompt 'second tail'
plan '{"plan":["RECALLED DRAFT VERIFIED"]}'
send '\r'; expect_turn "$send_offset" 'RECALLED DRAFT VERIFIED'
json "$root/peer/requests.jsonl" 'any(.. | objects; .text? == "draft å€\nsecond tail")'
terminal_close
# Empty Alt-Q must not append /quit or an empty entry after the latest record.
cp "$history" "$root/expected"
terminal_start
finish_alt_q
cmp "$history" "$root/expected"
# A persistence failure leaves both application and draft available to retry.
terminal_start
mv "$history" "$history.good"; mkdir "$history"
send 'recoverable draft\033q'; expect 'Draft not saved; staying open'
[[ ! -s "$term/status" ]]
expect_prompt '> recoverable draft'
rmdir "$history"; mv "$history.good" "$history"
finish_alt_q
[[ $(tail -1 "$history") == 'recoverable draft' ]]
[[ $(requests) == 1 ]]
# Exiting an active turn preserves the unsent draft without steering/submission.
terminal_start -N
plan '{"plan":[{"text":"BUSY DRAFT TEST","pause":true},"UNUSED"]}'
send 'start a turn\r'; expect 'STREAM in flight'
send 'busy unsent draft'
finish_alt_q
release
[[ $(tail -1 "$history") == 'busy unsent draft' && $(requests) == 2 ]]
absent "$root/peer/requests.jsonl" 'busy unsent draft'
terminal_start
send '\033[A'; expect_prompt '> busy unsent draft'
send '\025'; finish_alt_q
[[ $(tail -1 "$history") == 'busy unsent draft' && $(requests) == 2 ]]
# A different workspace has independent recall.
mkdir "$root/other"
base[${#base[@]}-1]="$root/other"
terminal_start -N
send '\033[A'; expect_prompt '> '
absent "$term/prompt" 'busy unsent draft'
finish_alt_q
[[ $(requests) == 2 ]]
# Escape followed by Alt-Q must leave the conversation resumable. Duplicate
# acknowledgements themselves have a deterministic core regression test.
base[${#base[@]}-1]="$work"
plan '{"plan":[{"text":"CANCEL AND REOPEN","pause":true}]}'
terminal_start -N
send 'Keep this conversation after cancellation\r'; expect 'STREAM in flight'
send '\033'; expect 'WARN: Turn stopped.'
finish_alt_q
release
plan '{"plan":["RESUMED AFTER ESCAPE AND ALT-Q"]}'
terminal_start
send 'Continue after reopening\r'
expect_turn "$send_offset" 'RESUMED AFTER ESCAPE AND ALT-Q'
contains "$root/peer/requests.jsonl" 'Keep this conversation after cancellation'
terminal_close
