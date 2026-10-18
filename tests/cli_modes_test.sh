#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# Git is a review input here, not a synthetic release-ref controller.
git -C "$work" init -q
git -C "$work" config core.hooksPath /dev/null
printf before >"$work/tracked.txt"; git -C "$work" add tracked.txt
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(cli): seed review input'
printf after >"$work/tracked.txt"
plan '{"plan":["first reply","second reply"]}'
run -Nni 'first task' -i 'second task'; ok
[[ $(stdout_text) == 'second reply' ]]
for text in 'first task' 'first reply' 'second task' 'second reply'; do contains "$root/stderr" "$text"; done
jq -se 'all(.[]; has("app")|not)' "$root/stderr" >/dev/null
jq -se 'any(.[]; .lvl == "info" and .role == "user" and .prompt_kind == "normal" and .msg == "first task") and any(.[]; .lvl == "info" and .role == "assistant" and .msg == "first reply")' "$root/stderr" >/dev/null
plan '{"plan":["resumed reply"]}'
run -ni 'resume task'; ok; [[ $(stdout_text) == 'resumed reply' ]]
contains "$root/peer/requests.jsonl" 'first task'
run --list; ok; [[ ! -s "$root/stderr" ]]; cp "$root/stdout" "$root/list"
id=$(awk 'NR==1 {print $1}' "$root/list")
[[ $(awk 'length>80 {print}' "$root/list") == '' ]]
contains "$root/list" 'first task'
for level in info debug; do
  export LOG_LEVEL="$level"
  run -l; ok; cmp "$root/stdout" "$root/list"
  jq -se "any(.[]; .lvl == \"$level\")" "$root/stderr" >/dev/null
  absent "$root/stderr" $'\033'
done
unset LOG_LEVEL
for option in --help --version --resume; do run "$option"; ok; [[ -s "$root/stdout" && ! -s "$root/stderr" ]]; done
run --nonexistent; bad; [[ ! -s "$root/stdout" ]]; jq -se 'any(.[]; .lvl == "error")' "$root/stderr" >/dev/null
count=$(requests)
run --export "$id"; ok
jsonl="$root/data/cai/exports/$id/$id.jsonl"; markdown="$root/data/cai/exports/$id/$id.md"
[[ -f "$jsonl" && -f "$markdown" && ! -e "$root/state/cai/exports" ]]
[[ $(stat -c %a "$jsonl") == 600 && $(stat -c %a "$markdown") == 600 ]]
jq -se '.[0].record_type == "checkpoint" and .[0].applied_event_sequence > 0 and any(.[]; .record_type == "event")' "$jsonl" >/dev/null
contains "$markdown" '## User'; contains "$markdown" 'first task'; contains "$markdown" 'resumed reply'
[[ $(requests) == "$count" ]]
run --export "$id" --export-dir relative-exports; ok; [[ -f "$work/relative-exports/$id/$id.md" ]]
mkdir "$root/import-work"; base[${#base[@]}-1]="$root/import-work"
run --import "$jsonl"; ok
new_id=$(awk 'NF {print $NF; exit}' "$root/stdout")
run --resume; ok; absent "$root/stdout" "$id"
run --list; ok; cp "$root/stdout" "$root/import-list"
for data in '' 'not json' '{"record_type":"event"}'; do
  printf '%s\n' "$data" >"$root/bad.jsonl"
  run --import "$root/bad.jsonl"; bad
  run --list; ok; cmp "$root/stdout" "$root/import-list"
done
base[${#base[@]}-1]="$work"
key="$root/state/cai/pouch.key"
[[ $(stat -c %a "$key") == 600 ]]
mv "$key" "$root/saved-key"
run --list; bad; contains "$root/stderr" 'restore the original key'; [[ ! -e "$key" ]]
mv "$root/saved-key" "$key"
run --list; ok
cp "$key" "$root/saved-key"; printf '%064d\n' 0 >"$key"
run --list; bad; cmp "$key" <(printf '%064d\n' 0)
mv "$root/saved-key" "$key"
run --lockd "pouch://$root/other-store" --list; ok; contains "$root/stdout" 'No conversations.'
# The storage round trips above are complete. Remaining assertions concern
# automation, review, stdout/stderr and errors, not session persistence.
export CAI_TEST_VOLATILE_SESSION=1
plan '{"plan":["working on goal","__complete_goal__","goal done"]}'
run -Nng 'finish fixture'; ok; [[ $(stdout_text) == 'goal done' ]]; contains "$root/stderr" 'working on goal'
contains "$root/peer/requests.jsonl" 'Continue pursuing the active goal'
plan '{"plan":["__complete_goal__","review loop done"]}'
run --review-and-fix; ok; [[ $(stdout_text) == 'review loop done' ]]
contains "$root/peer/requests.jsonl" run_review; contains "$root/peer/requests.jsonl" 'no actionable findings'
report=$(jq -c . "$support/fixtures/review.json")
plan "$(jq -n --arg report "$report" '{plan:["__run_review__",$report,"__complete_goal__","parent fix complete"]}')"
run --review-and-fix; ok; [[ $(stdout_text) == 'parent fix complete' ]]; absent "$root/stdout" 'Fix parser'
contains "$root/stderr" 'Fix parser'
plan '{"plan":["__summary__"]}'
run -Nni 'summarize fixture'; ok; [[ $(stdout_text) == 'summary reply' ]]
contains "$root/stderr" 'Short provider summary'
plan '{"plan":["previous completed answer","__api_error__"]}'
run -Nni 'first succeeds' -i 'second fails'; bad
[[ $(stdout_text) == 'previous completed answer' ]]
plan '{"plan":["__partial_failure__"]}'
run -Nni partial; bad; contains "$root/stdout" 'partial answer'
jq -se 'any(.[]; .event == "run_failed" and .lvl == "error")' "$root/stderr" >/dev/null
export LOG_LEVEL=warn
plan '{"plan":["quiet final"]}'
run -Nni 'quiet prompt'; ok; contains "$root/stdout" 'quiet final'; absent "$root/stderr" 'quiet prompt'
unset LOG_LEVEL
export LOG_MODE=console
plan '{"plan":["console final"]}'
run -Nni 'console prompt'; ok; contains "$root/stderr" INF; absent "$root/stderr" $'\033'
unset LOG_MODE
export LOG_OUTPUT=relative-activity.log
plan '{"plan":["relative activity final"]}'
run -Nni 'relative activity prompt'; ok; [[ ! -s "$root/stderr" ]]
[[ -f "$work/relative-activity.log" && ! -e "$root/relative-activity.log" ]]
contains "$work/relative-activity.log" 'relative activity prompt'
unset LOG_OUTPUT
for error in __http_error__ __api_error__; do
  plan "{\"plan\":[\"$error\"]}"
  run -Nni 'request must fail'; bad; [[ ! -s "$root/stdout" ]]; jq -e . "$root/stderr" >/dev/null
  if [[ "$error" == __api_error__ ]]; then
    for marker in 'HTTP 429' rate_limit_exceeded req_fixture_429 'quota exhausted'; do contains "$root/stderr" "$marker"; done
  else contains "$root/stderr" 'HTTP 500'; contains "$root/stderr" 'fixture error'; fi
done
# Exercise isolated review with the actual image-generation default, rather
# than inheriting the normal-agent fixture's capability override.
review_base=()
for arg in "${base[@]}"; do
  [[ "$arg" == --no-image-generation ]] || review_base+=("$arg")
done
base=("${review_base[@]}")
for mode in default disabled out json base custom activity; do
  items=$(jq -n --arg report "$report" '[$report]')
  flags=(--review)
  case "$mode" in
    disabled) flags+=(--no-image-generation) ;;
    out) flags+=(-o "$root/findings.md") ;;
    json) flags+=(-T json) ;;
    base) flags+=(--base HEAD) ;;
    custom) flags+=(-i 'Check the parser only') ;;
    activity) items=$(jq -n --arg report "$report" '["__read_file__",$report]') ;;
  esac
  plan "{\"plan\":$items}"
  run "${flags[@]}"; ok
  case "$mode" in
    out) [[ ! -s "$root/stdout" ]]; contains "$root/findings.md" '## Fix parser' ;;
    json) jq -Se . "$root/stdout" >"$root/actual"; jq -Se . "$support/fixtures/review.json" >"$root/expected"; cmp "$root/actual" "$root/expected" ;;
    *) contains "$root/stdout" '# Review findings' ;;
  esac
  contains "$root/stderr" 'patch is incorrect'
  if [[ "$mode" == activity ]]; then contains "$root/stderr" tool_call_started; contains "$root/stderr" tool_call_completed; fi
done
plan '{"plan":["not a review report"]}'
run --review; bad; [[ ! -s "$root/stdout" ]]
