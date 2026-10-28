#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
scope=${2:-full}
peer_start
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
mkdir -p "$root/config/cai"
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/config/cai/config.ini"
items() {
  local prepared
  prepared=$(jq -n --argjson chat "$chat" --argjson items "$1" '{chat:$chat,plan:$items}') || return 1
  plan "$prepared"
}
tool_names() { jq -r '.tools[]? | .name // .function.name // .type' "$1"; }
git -C "$work" init -q -b main
git -C "$work" config core.hooksPath /dev/null
printf 'before\n' >"$work/tracked.txt"
git -C "$work" add tracked.txt
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(review): seed tool scope'
if [[ "$scope" == smoke ]]; then
  # Exercise the Lua application's registration/dispatch/shared-library path.
  # Full review/patch/continuation failure modes belong to the native cases.
  terminal_start -N --review-and-fix-tool
  items '[{"tool":"run_review_and_fix","arguments":{"target":"uncommitted"}},{"tool":"update_goal","arguments":{"status":"complete"}},"FACADE LOOP DONE"]'
  send 'Run the review and fix loop\r'
  expect_assistant 'FACADE LOOP DONE'; expect 'goal=achieved'
  [[ $(requests) == 3 ]]
  contains "$term/text" 'review-and-fix goal'
  send '/goal\r'; expect 'Status: complete'; expect 'target=uncommitted'
  terminal_close
  exit 0
fi
[[ "$scope" == full ]]
items '["NORMAL REVIEW REQUEST"]'
run -Nni 'Review this change'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
tool_names "$root/request" >"$root/tools"
contains "$root/tools" run_review
absent "$root/tools" run_review_and_fix
contains "$root/request" 'A normal review request authorizes one review, not a review/fix loop'
contains "$root/request" 'If later asked to fix findings, fix and verify them without automatically reviewing again'
report=$(jq -c . "$support/fixtures/review.json")
clean=$(jq -c '.findings=[] | .overall_correctness="patch is correct" | .overall_explanation="No actionable findings"' "$support/fixtures/review.json")
# Having the tool available does not turn review + later fix into a loop.
items "$(jq -n --arg report "$report" '[{tool:"run_review",arguments:{target:"uncommitted"}},$report,"SINGLE REVIEW DONE","FINDINGS FIXED"]')"
before=$(requests)
run -Nn --review-and-fix-tool -i 'Review the uncommitted changes' -i 'Fix those findings'; ok
[[ $(stdout_text) == 'FINDINGS FIXED' && $(requests) == $((before+4)) ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'Enabling a tool is not that authorization'
contains "$root/request" 'A normal review request, even followed by a request to fix findings, is not permission'
# A real patch, a second reviewer and automatic goal continuation all run in
# the same conversation; the optional tool only starts that shared workflow.
terminal_start -N --review-and-fix-tool
patch=$'*** Begin Patch\n*** Update File: tracked.txt\n@@\n-before\n+after\n*** End Patch\n'
items "$(jq -n --argjson chat "$chat" --arg report "$report" --arg clean "$clean" --arg patch "$patch" '[
  {tool:"run_review_and_fix",arguments:{}},
  {tool:"run_review",arguments:{}},$report,
  {tool:"apply_patch",arguments:(if $chat then {input:$patch} else $patch end)},"FIXED; NEXT REVIEW PENDING",
  {tool:"run_review",arguments:{}},$clean,
  {tool:"update_goal",arguments:{status:"complete"}},"REVIEW FIX LOOP DONE"]')"
before=$(requests)
send 'Run review and fix in a loop until clean\r'
expect_assistant 'REVIEW FIX LOOP DONE'; expect 'goal=achieved'
[[ $(requests) == $((before+9)) && $(cat "$work/tracked.txt") == after ]]
contains "$term/text" 'review-and-fix goal'
absent "$term/feed" '{"status":"queued"}'
sed -n "$((before+3))p" "$root/peer/requests.jsonl" >"$root/child"
tool_names "$root/child" >"$root/tools"
absent "$root/tools" run_review_and_fix
absent "$root/tools" run_review
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'User-authorized review/fix loop'
contains "$root/request" 'Review changes against base refs/heads/main'
send '/goal\r'; expect 'Status: complete'
# Bad scope must fail without replacing the completed goal or starting pursuit.
for args in '{"target":"base"}' '{"target":"uncommitted","base":"HEAD"}' '{"target":"base","base":"--unsafe"}'; do
  items "$(jq -n --argjson args "$args" '[{tool:"run_review_and_fix",arguments:$args},"INVALID SCOPE HANDLED"]')"
  before=$(requests)
  send 'Exercise the invalid scope of the requested loop\r'
  expect_turn "$send_offset" 'INVALID SCOPE HANDLED'
  [[ $(requests) == $((before+2)) ]]
  send '/goal\r'; expect 'Status: complete'
done
# A loop started by the tool uses ordinary Escape/pause behavior.
items '[{"tool":"run_review_and_fix","arguments":{"target":"uncommitted"}},{"pause":true,"text":"UNWANTED TAIL"}]'
send 'Run the review and fix loop again\r'; expect 'STREAM in flight'
send '\033'; expect 'Turn stopped.'; expect 'goal=paused'; release
terminal_close
for disabled in --no-goals --no-review-subagent; do
  before=$(requests)
  run -Nni 'Must not sample' --review-and-fix-tool "$disabled"; bad
  [[ $(requests) == "$before" ]]
  contains "$root/stderr" 'run_review_and_fix requires goal tools'
done
