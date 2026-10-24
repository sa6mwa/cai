#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_VOLATILE_SESSION=1
git -C "$work" init -q -b main
git -C "$work" config core.hooksPath /dev/null
printf 'base\n' >"$work/tracked.txt"
git -C "$work" add tracked.txt
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(review): seed review-and-fix scope'
git -C "$work" checkout -qb feature/review
report=$(jq -c . "$support/fixtures/review.json")
clean=$(jq -c '.findings=[] | .overall_correctness="patch is correct" | .overall_explanation="No actionable findings"' "$support/fixtures/review.json")
terminal_start -N
send '/help\r'; expect '/review-and-fix'
for args in base 'base HEAD extra' nonsense; do
  send "/review-and-fix $args\r"; expect 'Use /review-and-fix'
done
[[ $(requests) == 0 ]]
plan '{"plan":["ANCHOR CONTEXT"]}'
prompt 'Remember this conversation anchor' 'ANCHOR CONTEXT'
# The explicit command replaces the existing goal in this conversation.
plan '{"plan":[{"text":"OLD GOAL","pause":true}]}'
send '/goal new Previous objective\r'; expect 'STREAM in flight'
send '/goal pause\r'; release; expect paused
plan "$(jq -n --arg report "$report" --arg clean "$clean" '{plan:[{tool:"run_review",arguments:{}},$report,{tool:"run_review",arguments:{}},$clean,"__complete_goal__","FIX LOOP DONE"]}')"
before=$(requests)
send '/review-and-fix\r'; expect_assistant 'FIX LOOP DONE'; expect 'Worked for'
[[ $(requests) == $((before+6)) ]]
sed -n "$((before+2))p" "$root/peer/requests.jsonl" >"$root/reviewer"
contains "$root/reviewer" "base branch 'refs/heads/main'"
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'Remember this conversation anchor'
contains "$root/request" 'Previous objective'
contains "$root/request" 'Review changes against base refs/heads/main'
contains "$root/request" 'No actionable findings'
absent "$root/request" '/review-and-fix'
send '/goal\r'; expect complete; expect 'Review changes against base'
# A missing or ambiguous default fails locally; explicit scopes still work.
git -C "$work" branch master
before=$(requests)
send '/review-and-fix\r'; expect 'unambiguous Git default branch'
[[ $(requests) == "$before" ]]
for scope in uncommitted 'base  HEAD  '; do
  plan '{"plan":["__complete_goal__","SCOPED FIX DONE"]}'
  send "/review-and-fix $scope\r"; expect_assistant 'SCOPED FIX DONE'; expect 'Worked for'
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  if [[ "$scope" == uncommitted ]]; then
    contains "$root/request" 'target=uncommitted'
  else
    contains "$root/request" 'Review changes against base HEAD'
  fi
done
plan '{"plan":["AFTER FIX LOOP"]}'
prompt 'Continue this conversation' 'AFTER FIX LOOP'
terminal_close
for disabled in --no-review-subagent --no-goals; do
  terminal_start -N "$disabled"
  before=$(requests)
  send '/review-and-fix\r'
  if [[ "$disabled" == --no-goals ]]; then
    expect 'Goal pursuit requires goal tools'
  else
    expect 'review subagent is disabled'
  fi
  [[ $(requests) == "$before" ]]
  terminal_close
done
