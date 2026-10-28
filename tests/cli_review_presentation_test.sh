#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
scope=${2:-lifecycle}
# libmdf chooses the advertised 256-color palette; Softline uses RGB. Keep
# those independent so a sticky-line redraw cannot masquerade as a theme.
unset COLORTERM
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false; [[ "$protocol" != chat_completions ]] || chat=true
report=$(jq -c . "$support/fixtures/review.json")
ready() { [[ $(requests) == "$expected_requests" ]]; }
palette() {
  local label=$1 expected_offset=${2:-0} offset
  offset=$(wc -c <"$term/raw")
  send '/theme-offset\r'; expect "Parent theme offset: $expected_offset."
  { tail -c +$((offset+1)) "$term/raw" | LC_ALL=C grep -oE '38;5;[0-9]+' || :; } |
    LC_ALL=C sort -u >"$root/$label.palette"
}
review_plan() {
  plan "$(jq -nc --argjson chat "$chat" --arg report "$report" '{chat:$chat,plan:[{pause:true,pause_text:"",text:$report}]}')"
}
if [[ "$scope" == plain ]]; then
  for mode in ascii no_color same_theme; do
    extra=()
    case "$mode" in
      no_color) export NO_COLOR=1; theme=dracula ;;
      same_theme) theme=dracula; extra=(--review-theme-offset 0) ;;
      *) theme=plain ;;
    esac
    review_plan
    terminal_start -N -t "$theme" "${extra[@]}"
    expected_requests=$(( $(requests)+1 ))
    send '/review custom Inspect the fixture\r'
    expect 'Review started'; wait_for ready
    release; expect 'Review finished'
    contains "$term/text" $'  Review started\n'
    contains "$term/text" $'  Review finished\n'
    if [[ "$mode" != same_theme ]]; then absent "$term/raw" '38;'; fi
    terminal_close
    unset NO_COLOR
  done
  exit 0
fi
export CAI_TEST_STEER_WORKER=1
terminal_start -N -t dracula
send '/review-theme-offset\r'; expect 'Review theme offset: 1.'
send '/subagent-theme-offset\r'; expect 'Subagent theme offset: 0.'
palette parent
[[ -s "$root/parent.palette" ]]
plan "$(jq -nc --argjson chat "$chat" --arg report "$report" '{chat:$chat,plan:[
 {tool:"run_review",arguments:{target:"custom",instructions:"Inspect the fixture"}},
 {pause:true,pause_text:"",text:$report},"**REVIEW TOOL DONE** with `parent code`."]}')"
expected_requests=$(( $(requests)+2 ))
send 'review once\r'; expect '* Starting run_review'; expect 'Review started'; wait_for ready
absent "$term/text" '* Completed run_review'
palette reviewer
if cmp -s "$root/parent.palette" "$root/reviewer.palette"; then fail 'review kept parent palette'; fi
release; expect 'REVIEW TOOL DONE'; expect 'Worked for'
palette restored
cmp "$root/parent.palette" "$root/restored.palette"
start=$(grep -nFm1 '* Starting run_review' "$term/text" | cut -d: -f1)
handoff=$(grep -nFm1 'Review handoff' "$term/text" | cut -d: -f1)
end=$(grep -nFm1 '* Completed run_review' "$term/text" | cut -d: -f1)
finished=$(grep -nFm1 '  Review finished' "$term/text" | cut -d: -f1)
[[ "$start" -lt "$handoff" && "$handoff" -lt "$finished" && "$finished" -lt "$end" ]]
# Failed preparation still has a start receipt, and never changes the palette.
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
 {tool:"run_review",arguments:{target:"base",base:"missing-review-base"}},"FAILED PREPARATION DONE"]}')"
prompt 'review invalid base' 'FAILED PREPARATION DONE'
contains "$term/text" '* Failed run_review'
palette failed
cmp "$root/parent.palette" "$root/failed.palette"
# Manual review uses the same styling, including cancellation recovery.
for outcome in complete cancel; do
  review_plan
  expected_requests=$(( $(requests)+1 ))
  send '/review custom Inspect the fixture\r'
  expect 'Review started'; wait_for ready
  palette manual
  cmp "$root/reviewer.palette" "$root/manual.palette"
  if [[ "$outcome" == cancel ]]; then
    send '\033'; expect 'Review stopped.'
  fi
  release; expect 'Review finished'
  palette after_manual
  cmp "$root/parent.palette" "$root/after_manual.palette"
done
# Ordinary children inherit the parent; an explicit offset changes only MDF.
for offset in 0 1234; do
  send "/subagent-theme-offset $offset\r"; expect "Subagent theme offset: $offset."
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
    {tool:"run_worker",arguments:{instructions:"Inspect the fixture"}},
    {pause:true,pause_text:"",text:"WORKER BODY"},"WORKER DONE"]}')"
  expected_requests=$(( $(requests)+2 ))
  send 'delegate once\r'; expect '* Starting run_worker'; wait_for ready
  palette worker
  if [[ "$offset" == 0 ]]; then cmp "$root/parent.palette" "$root/worker.palette";
  elif cmp -s "$root/parent.palette" "$root/worker.palette"; then fail 'child offset had no effect'; fi
  release; expect 'WORKER DONE'; expect 'Worked for'
done
send '/theme-offset 2\r'; expect 'Parent theme offset: 2.'
palette parent_two 2
send '/theme-offset 1\r'; expect 'Parent theme offset: 1.'
palette parent_one 1
cmp "$root/reviewer.palette" "$root/parent_one.palette"
review_plan
expected_requests=$(( $(requests)+1 ))
send '/review custom Check relative offsets\r'; expect 'Review started'; wait_for ready
palette nested 1
cmp "$root/parent_two.palette" "$root/nested.palette"
release; expect 'Review finished'
palette after_nested 1
cmp "$root/parent_one.palette" "$root/after_nested.palette"
send '/theme-offset 0\r'; expect 'Parent theme offset: 0.'
for setting in theme-offset subagent-theme-offset review-theme-offset; do
  send "/$setting -300\r"; expect 'theme offset: -300.'
  send "/$setting 9223372036854775808\r"; expect 'signed 64-bit integer'
  send "/$setting\r"; expect 'theme offset: -300.'
  send "/$setting 0\r"; expect 'theme offset: 0.'
done
id=$(current_session_id)
[[ $(grep -c '^  Review started$' "$term/feed") == 4 ]]
[[ $(grep -c '^  Review finished$' "$term/feed") == 4 ]]
before=$(requests)
send '/review base missing-review-base\r'
expect 'Review started'; expect 'Review finished'
expect 'failed to resolve the review merge base'
palette manual_preparation_failed
cmp "$root/parent.palette" "$root/manual_preparation_failed.palette"
[[ $(requests) == "$before" ]]
terminal_close
before=$(requests)
terminal_start --resume "$id" -t dracula --replay-turns -1
expect 'Conversation resumed.'
contains "$term/text" 'Review subagent'
[[ $(grep -c '^  Review started$' "$term/text") == 4 ]]
[[ $(grep -c '^  Review finished$' "$term/text") == 4 ]]
LC_ALL=C grep -oE '38;5;[0-9]+' "$term/raw" | LC_ALL=C sort -u >"$root/replay.palette"
while IFS= read -r color; do contains "$root/replay.palette" "$color"; done <"$root/reviewer.palette"
palette replay_restored
cmp "$root/parent.palette" "$root/replay_restored.palette"
terminal_close
[[ $(requests) == "$before" ]]
