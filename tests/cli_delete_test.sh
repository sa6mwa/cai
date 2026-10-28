#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
session_id() { current_session_id; }
history_file() { find "$root/state/cai/history/$1" -name '*.history' -print; }

# An open conversation is omitted, and an explicit deletion fails unchanged.
terminal_start -N
first=$(session_id)
[[ -n "$first" ]]
plan '{"plan":["FIRST SAVED ANSWER"]}'
prompt 'First saved prompt' 'FIRST SAVED ANSWER'
history=$(history_file "$first")
[[ -s "$history" && -s "$root/cache/cai/$first.log" ]]
run -D; ok; contains "$root/stdout" 'No conversations.'
run --delete "$first"; bad; contains "$root/stderr" 'currently open'
[[ -s "$history" && -s "$root/cache/cai/$first.log" ]]

# Switching sessions releases the old use guard and opens a distinct history.
send '/new\r'; expect 'New session'; expect 'ID:'
second=$(session_id)
[[ "$second" != "$first" ]]
run --export "$first"; ok
exports="$root/data/cai/exports/$first"
[[ -s "$exports/$first.jsonl" && -s "$exports/$first.md" ]]
printf sentinel >"$root/sentinel"
ln -s "$root/sentinel" "$(dirname "$history")/outside-link"
printf workspace >"$work/keep-me"
# A filesystem cleanup error leaves the stored conversation selectable, and
# retrying the same ID completes cleanup after the problem is repaired.
mv "$root/cache/cai/$first.log" "$root/first-log"
mkdir "$root/cache/cai/$first.log"
run -D "$first"; bad; contains "$root/stderr" 'remove conversation data'
run -R; ok; contains "$root/stdout" "$first"
rmdir "$root/cache/cai/$first.log"
mv "$root/first-log" "$root/cache/cai/$first.log"
run --delete; ok
contains "$root/stdout" "1  $first"
absent "$root/stdout" "$second"
run -D 1; ok; contains "$root/stdout" "Deleted conversation $first."
[[ ! -e "$(dirname "$history")" && ! -e "$exports" && ! -e "$root/cache/cai/$first.log" ]]
[[ $(cat "$root/sentinel") == sentinel && $(cat "$work/keep-me") == workspace ]]
[[ $(session_id) == "$second" ]]
run --delete "$first"; bad; contains "$root/stderr" 'was not found'
terminal_close

# Invalid choices and directory mismatches never remove another conversation.
for choice in 0 999999999999999999999999999999999 ../sentinel missing-id; do
  run -D "$choice"; bad
done
run --delete ''; bad
mkdir "$root/elsewhere"
run -C "$root/elsewhere" -D "$second"; bad
contains "$root/stderr" 'another directory'
[[ $(session_id) == "$second" ]]

# Deleting the newest repairs auto-resume to the remaining checkpoint.
terminal_start -N
newest=$(session_id)
terminal_close
run --delete "$newest"; ok
[[ $(session_id) == "$second" ]]
terminal_start
send '/status\r'; expect "$second"
terminal_close

# Process death releases use without a lease timeout or heartbeat.
terminal_start
run -D "$second"; bad; contains "$root/stderr" 'currently open'
application=$(awk '{print $3}' "$term/tty")
[[ "$application" -gt 1 ]]
kill -KILL "$application"
wait_for test -s "$term/status"
wait "$terminal_pid" || :
terminal_pid=
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
run -D; ok; contains "$root/stdout" "1  $second"
run --delete "$second"; ok
run -R; ok; contains "$root/stdout" 'No conversations.'
[[ $(requests) == 1 ]]

# Real runtime-created reviewer sessions are owned by the parent deletion,
# for both a model-launched subagent and a manual /review invocation.
report=$(jq -c . "$support/fixtures/review.json")
plan "$(jq -n --arg report "$report" '{plan:[{tool:"run_review",arguments:{instructions:"Inspect the fixture"}},$report,"REVIEW PARENT DONE"]}')"
terminal_start -N
parent=$(session_id)
prompt 'Delegate a fixture review' 'REVIEW PARENT DONE'
plan "$(jq -n --arg report "$report" '{plan:[$report]}')"
send '/review Inspect the fixture\r'; expect 'Fix parser'; expect 'Worked for'
run -l; ok
[[ $(wc -l <"$root/stdout") == 3 ]]
terminal_close
run --delete "$parent"; ok
run -l; ok; contains "$root/stdout" 'No conversations.'
[[ $(requests) == 5 ]]
