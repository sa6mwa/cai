#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export LOG_LEVEL=warn CAI_TEST_VOLATILE_SESSION=1
git -C "$work" init -q -b main
git -C "$work" config core.hooksPath /dev/null
printf 'base\n' >"$work/tracked.txt"
git -C "$work" add tracked.txt
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(review): seed default branch'
base_commit=$(git -C "$work" rev-parse HEAD)
git -C "$work" checkout -qb feature/review
printf 'working changes\n' >"$work/tracked.txt"
report=$(jq -c . "$support/fixtures/review.json")
review_plan() { plan "$(jq -n --arg report "$report" '{plan:[$report]}')"; }
last_request() { tail -1 "$root/peer/requests.jsonl" >"$root/request"; }
for branch in main master trunk; do
  if [[ "$branch" == master ]]; then git -C "$work" branch -m main master; fi
  if [[ "$branch" == trunk ]]; then git -C "$work" branch -m master trunk; fi
  review_plan
  run --review; ok; contains "$root/stdout" '# Review findings'
  last_request
  contains "$root/request" "base branch 'refs/heads/$branch'"
  contains "$root/request" "git diff $base_commit"
done
# Remote HEAD wins even when conventional local names are ambiguous.
git -C "$work" branch main
git -C "$work" update-ref refs/remotes/origin/develop "$base_commit"
git -C "$work" symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/develop
review_plan
GIT_DIR="$root/not-a-repo" GIT_WORK_TREE="$root" run --review; ok
last_request; contains "$root/request" "base branch 'refs/remotes/origin/develop'"
# Explicit scopes bypass default discovery, including in an ambiguous repo.
git -C "$work" symbolic-ref --delete refs/remotes/origin/HEAD
count=$(requests)
run --review; bad; contains "$root/stderr" 'unambiguous Git default branch'
run --review-and-fix; bad
[[ $(requests) == "$count" ]]
for mode in uncommitted base custom; do
  review_plan
  case "$mode" in
    uncommitted) run --review uncommitted ;;
    base) run --review --base HEAD ;;
    custom) run --review -i 'Only inspect the parser' ;;
  esac
  ok; last_request
  case "$mode" in
    uncommitted) contains "$root/request" 'staged, unstaged, and untracked' ;;
    base) contains "$root/request" "base branch 'HEAD'" ;;
    custom) contains "$root/request" 'Only inspect the parser' ;;
  esac
done
run --review uncommitted --base HEAD; bad
run --review uncommitted -i 'conflicting scope'; bad
run --review-and-fix uncommitted --base HEAD; bad
git -C "$work" branch -D main >/dev/null
# A bare tool call uses the same default; explicit uncommitted stays explicit.
for arguments in '{}' '{"target":"uncommitted"}' '{"base":"HEAD"}' '{"instructions":"Custom tool scope"}'; do
  plan "$(jq -n --argjson args "$arguments" --arg report "$report" '{plan:[{tool:"run_review",arguments:$args},$report,"REVIEW DONE"]}')"
  before=$(requests)
  run -Nni 'Review this branch'; ok
  sed -n "$((before+2))p" "$root/peer/requests.jsonl" >"$root/request"
  case "$arguments" in
    '{}') contains "$root/request" "base branch 'refs/heads/trunk'" ;;
    *uncommitted*) contains "$root/request" 'staged, unstaged, and untracked' ;;
    *base*) contains "$root/request" "base branch 'HEAD'" ;;
    *) contains "$root/request" 'Custom tool scope' ;;
  esac
done
plan '{"plan":["__complete_goal__","FIX LOOP DONE"]}'
run --review-and-fix; ok
last_request; contains "$root/request" 'Review changes against base refs/heads/trunk'
plan '{"plan":["__complete_goal__","FIX LOOP DONE"]}'
run --review-and-fix uncommitted; ok
last_request; contains "$root/request" 'target=uncommitted'
# Non-repositories and repositories without known defaults fail before inference.
git -C "$work" branch -m trunk unusual-default
count=$(requests)
run --review; bad; [[ $(requests) == "$count" ]]
mkdir "$root/plain"
# The fixture lives below the owning Git checkout; stop ancestor discovery.
printf 'gitdir: missing\n' >"$root/plain/.git"
run -C "$root/plain" --review; bad; contains "$root/stderr" 'requires a Git repository'
[[ $(requests) == "$count" ]]
review_plan
run -C "$root/plain" --review uncommitted; ok
# TUI review runs directly in an isolated child, then returns context to parent.
git -C "$work" branch -m unusual-default trunk
review_plan
terminal_start -N
before=$(requests)
send '/review\r'; expect 'Fix parser'
[[ $(requests) == $((before+1)) ]]
last_request; contains "$root/request" "base branch 'refs/heads/trunk'"
plan '{"plan":["AFTER REVIEW"]}'
prompt 'Explain the findings' 'AFTER REVIEW'
last_request; contains "$root/request" 'Fix parser'
review_plan
send '/review uncommitted\r'; expect 'Fix parser'
last_request; contains "$root/request" 'staged, unstaged, and untracked'
# Queued input waits for the isolated review's durable handoff.
plan "$(jq -n --arg report "$report" '{plan:[{pause:true,pause_text:"",reasoning:["REVIEW QUEUE BARRIER"],text:$report},"QUEUED AFTER REVIEW"]}')"
before=$(requests)
send '/review\r'; expect 'REVIEW QUEUE BARRIER'
send '/review uncommitted\r'; expect 'finish the active review'
send '/review-and-fix\r'; expect 'finish the active review'
send '/shell\r'; expect 'Shell is available only between turns.'
send '\025'
send '/status\r'; expect 'Metric'
send 'Use the queued review findings\t'
[[ $(requests) == $((before+1)) ]]
release
expect 'QUEUED AFTER REVIEW'
last_request; contains "$root/request" 'Fix parser'
# Cancelling an active child releases its parent for subsequent normal work.
plan "$(jq -n --arg report "$report" '{plan:[{pause:true,pause_text:"",reasoning:["REVIEW CANCEL BARRIER"],text:$report}]}')"
send '/review\r'; expect 'REVIEW CANCEL BARRIER'
send '\033'; expect 'Worked for'
plan '{"plan":["AFTER CANCELLED REVIEW"]}'
prompt 'Continue after cancellation' 'AFTER CANCELLED REVIEW'
# Invalid reviewer reports are handed back as failure context, without killing the UI.
plan '{"plan":["not a review report"]}'
send '/review\r'; expect 'required JSON report schema'
plan '{"plan":["AFTER FAILED REVIEW"]}'
prompt 'Continue after the failed review' 'AFTER FAILED REVIEW'
terminal_close
# Closing with an active child cancels and closes it before the parent.
plan "$(jq -n --arg report "$report" '{plan:[{pause:true,pause_text:"",reasoning:["REVIEW QUIT BARRIER"],text:$report}]}')"
terminal_start -N
send '/review\r'; expect 'REVIEW QUIT BARRIER'
terminal_close
