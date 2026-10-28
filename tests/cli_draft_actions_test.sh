#!/usr/bin/env bash
# Exercise Softline's native editor through the real application owner loop.
source "$(dirname "$0")/cli_test_common.sh"
scope=${1:-editing}
peer_start
not_visible() { ! grep -Fq -- "$2" "$1"; }
empty_draft() { grep -Eq '^>[[:space:]]*$' "$term/prompt"; }
plain_draft() { ! grep -Eq '[QSH] editing' "$term/message"; }
quit_keys() {
  send "$1\033q"
  expect '! Goodbye.'
  release
  wait_for test -s "$term/status"
  wait "$terminal_pid"; terminal_pid=
  [[ $(cat "$term/status") == 0 && ! -s "$term/stderr" ]]
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD
}
if [[ "$scope" == editing ]]; then
  terminal_start -N
  # Stash is independent of the kill ring, reusable, and unaffected by an
  # empty stash operation. Append is at the end, rather than at the cursor.
  send 'STASH\033k'; wait_for empty_draft
  send 'KILL\025\033j\031'; expect_prompt STASHKILL
  send '\033n'; wait_for empty_draft
  send '\037'; expect_prompt STASHKILL
  send '\033n'; wait_for empty_draft
  send '\033k\033j'; expect_prompt STASH
  send '\001prefix-\001\033j'; expect_prompt prefix-STASHSTASH
  send '\033n'; wait_for empty_draft
  # Multiline/Unicode text survives a whole-draft stash and submission. The
  # next prompt can retrieve the same stash; stashing itself sends nothing.
  send 'FIRST λ\012SECOND β\033k'; wait_for empty_draft
  [[ $(requests) == 0 ]]
  plan '{"plan":["DRAFT ACCEPTED"]}'
  send '\033j\r'; expect_turn "$send_offset" 'DRAFT ACCEPTED'
  json "$root/peer/requests.jsonl" 'any(..|strings; .=="FIRST λ\nSECOND β")'
  send '\033j'; expect_prompt SECOND
  send '\033n'; wait_for empty_draft
  # Applying a theme must neither leak SGR bytes into the draft nor reset its
  # stash. CAI deliberately leaves Alt-K/J/N unbound for Softline to handle.
  send 'THEME-DRAFT\033k'; wait_for empty_draft
  send '/theme gruvbox\r'; expect 'Theme changed to gruvbox:gruvbox.'
  send '\033j'; expect_prompt THEME-DRAFT
  absent "$term/prompt" '[0m'; absent "$term/prompt" '38;'
  send '\033n'; wait_for empty_draft
  plan '{"plan":[{"pause":true,"pause_text":"DRAFT BUSY\n\n","text":"DRAFT FINISHED"}]}'
  send 'Wait while I edit queued input\r'; expect 'DRAFT BUSY'
  before=$(requests)
  for mode in queued steer; do
    if [[ "$mode" == queued ]]; then submit='\r'; else submit='\033\r'; fi
    send "DELETE_$mode$submit\033e"; expect_prompt "DELETE_$mode"
    send '\033n'; wait_for empty_draft
    wait_for not_visible "$term/message" "DELETE_$mode"
    send '\037'; expect_prompt "DELETE_$mode"
    wait_for plain_draft
    send '\033n'; wait_for empty_draft
    send "EMPTY_$mode$submit\033e"; expect_prompt "EMPTY_$mode"
    send "\025$submit"; wait_for empty_draft
    wait_for not_visible "$term/message" "EMPTY_$mode"
  done
  # Stashing a queued edit retains its native identity/position until explicit
  # discard; undo after discard restores text without that queue identity.
  send 'STASH_QUEUE\r\033e'; expect_prompt STASH_QUEUE
  send '\033k'; wait_for empty_draft
  wait_for has "$term/message" 'Q editing'
  send '\033j'; expect_prompt STASH_QUEUE
  send '\033n'; wait_for empty_draft
  send '\037'; expect_prompt STASH_QUEUE; wait_for plain_draft
  send '\033n'; wait_for empty_draft
  # Escape still cancels editing and restores the original queued text, while
  # CAI cancels the active turn. It must not be treated as explicit discard.
  send 'KEEP_ORIGINAL\r\033e'; expect_prompt KEEP_ORIGINAL
  send '\025replacement\033'; expect 'WARN: Turn stopped.'
  release
  expect 'KEEP_ORIGINAL'
  send '\033e'; expect_prompt KEEP_ORIGINAL
  send '\033n'; wait_for empty_draft
  wait_for not_visible "$term/message" KEEP_ORIGINAL
  [[ $(requests) == "$before" ]]
  terminal_close
  absent "$root/peer/requests.jsonl" DELETE_queued
  absent "$root/peer/requests.jsonl" DELETE_steer
  absent "$root/peer/requests.jsonl" EMPTY_queued
  absent "$root/peer/requests.jsonl" EMPTY_steer
  exit 0
fi
[[ "$scope" == approvals ]]
terminal_start -N --messaging --message-queue draft-inbox
id=$(current_session_id)
[[ -n "$id" ]]
incoming() {
  "$cli" -C "$work" -q draft-inbox -M "$1" >"$root/sent" 2>"$root/send-log"
  json "$root/sent" '.status=="queued"'
  expect 'approval'
}
# With an ordinary draft present, Alt-N clears only that draft. Stash it to
# free the empty input boundary where native approval focus becomes available.
send 'LOCAL_DRAFT'; expect_prompt LOCAL_DRAFT
incoming FOCUSED_REJECT
send '\033n'; wait_for empty_draft
wait_for has "$term/message" FOCUSED_REJECT
send '\037'; expect_prompt LOCAL_DRAFT
send '\033k'; wait_for empty_draft
send '\033a'; expect 'Alt-Y approve'
wait_for has "$term/message" FOCUSED_REJECT
send '\033n'
wait_for not_visible "$term/message" FOCUSED_REJECT
send '\033j'; expect_prompt LOCAL_DRAFT
send '\033n'; wait_for empty_draft
for action in discard empty; do
  incoming "EDITED_$action"
  send '\033a\033e'; expect_prompt "EDITED_$action"
  if [[ "$action" == discard ]]; then
    # Discard and quit in one input burst, before idle polling can persist it.
    quit_keys '\033n'
  else
    # Whole-draft stash clears a multiline agent message while keeping its
    # held identity. Empty Enter then explicitly deletes it.
    send '\033k'; wait_for empty_draft
    quit_keys '\r'
  fi
  terminal_start --resume "$id" --messaging --message-queue draft-inbox
  # Arrival ordering is an observable recovery barrier: an older resurrected
  # message would be offered before this fresh canary. No sleeps or inference.
  incoming "CANARY_$action"
  send '\033a'; expect 'Alt-Y approve'
  wait_for has "$term/message" "CANARY_$action"
  send '\033n'; wait_for not_visible "$term/message" "CANARY_$action"
  send '/queue off\r'; expect 'Messaging is disabled'
  absent "$term/text" "EDITED_$action"
  if [[ "$action" == discard ]]; then
    send '/queue on\r'; expect 'Message queue: draft-inbox'
  fi
done
[[ $(requests) == 0 ]]
# An untouched, actively edited approval must survive teardown unchanged.
send '/queue on\r'; expect 'Message queue: draft-inbox'
incoming SURVIVES_QUIT
send '\033a\033e'; expect_prompt SURVIVES_QUIT
send ' UNSAVED_CHANGE'
quit_keys ''
terminal_start --resume "$id" --messaging --message-queue draft-inbox
expect 'approval'
send '\033a'; expect 'Alt-Y approve'
wait_for has "$term/message" SURVIVES_QUIT
absent "$term/message" UNSAVED_CHANGE
send '\033n'
send '/queue off\r'; expect 'Messaging is disabled'
terminal_close
