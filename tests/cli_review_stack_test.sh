#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_SMALL_STACK=1
git -C "$work" init -q -b feature/review
git -C "$work" config core.hooksPath /dev/null
printf 'seed\n' >"$work/tracked.txt"
git -C "$work" add tracked.txt
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(review): seed isolated scope'
report=$(jq -c . "$support/fixtures/review.json")
# Keep the real encrypted store: the second tool opens a fresh review scope,
# used to rebuild a latest-session index and overflow in LQL on the parent's
# worker. It must now open without that index; volatile fixtures mask the bug.
plan "$(jq -nc --arg report "$report" '{plan:[
  {tool:"run_review",arguments:{}},
  {tool:"run_review",arguments:{target:"base",base:"HEAD"}},$report,
  "REVIEW SURVIVED"]}')"
run -Nni 'review and fix findings'; ok
[[ $(stdout_text) == 'REVIEW SURVIVED' && $(requests) == 4 ]]
contains "$root/stderr" 'cannot resolve an unambiguous Git default branch'
id=$(current_session_id)
# Reopen the conversation, then open another reviewer against populated
# storage. Parent/child commit and replay paths must remain usable.
plan "$(jq -nc --arg report "$report" '{plan:[
  {tool:"run_review",arguments:{target:"custom",instructions:"Inspect the fixture"}},
  $report,{pause:true,text:"SECOND REVIEW SURVIVED"}]}')"
terminal_start --resume "$id"
send 'review once more\r'
# The report must reach the live Markdown feed before the parent supplies any
# answer. A batch-only assertion previously missed this dropped handoff.
expect 'Fix parser'
expect 'Reject bad input'
absent "$term/text" 'overall_correctness'
absent "$term/text" 'handover_markdown'
release
expect 'SECOND REVIEW SURVIVED'; expect 'Worked for'
[[ $(requests) == 7 ]]
terminal_close
before=$(requests)
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" 'SECOND REVIEW SURVIVED'
contains "$term/text" '# Review handoff'
terminal_close
[[ $(requests) == "$before" ]]
