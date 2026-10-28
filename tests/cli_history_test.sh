#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
history_reader=$(realpath "$1")
peer_start
terminal_start -N
first=$(current_session_id)
plan '{"plan":[{"text":"HistoryFirstDone","pause":true},"HistorySteerDone"]}'
offset=$(wc -c <"$term/text")
send '\033[200~history unicode λ\nsecond \\ tab\t\033[201~\r'; expect 'STREAM in flight'
prompt_history_dump "$first" >"$root/prompts"
json "$root/prompts" '.prompt == "history unicode λ\nsecond \\ tab\t"'
[[ ! -e "$root/state/cai/history" && ! -e "$root/state/softline" ]]
has_steer() { prompt_history_dump "$first" >"$root/prompts"; grep -Fq history-steering "$root/prompts"; }
send 'history-steering\033\r'; wait_for has_steer
release; expect_turn "$offset" HistorySteerDone
prompt_history_dump "$first" >"$root/prompts"
json "$root/prompts" .
[[ $(jq -s '[.[] | select(.prompt == "history-steering")] | length' "$root/prompts") == 1 ]]
count=$(requests)
send '/new\r'; expect 'New session'; expect 'ID:'; expect 'Workspace:'; expect '$ /new'
absent "$term/feed" '--- session ---'
second=$(current_session_id)
[[ "$second" != "$first" ]]
send '\020'; expect_prompt '> '
absent "$term/prompt" history-steering
send '/goal\r'; expect 'No goal.'
prompt_history_dump "$second" >"$root/prompts"
json "$root/prompts" '.prompt == "/goal"'
send '/resume 2\r'; expect 'HistorySteerDone'
prompt_history_dump "$first" >"$root/snapshot"
send '\022history-steering\r'; expect_prompt '> history-steering'
[[ $(requests) == "$count" ]]
prompt_history_dump "$first" >"$root/prompts"; cmp "$root/prompts" "$root/snapshot"
send '\025'; terminal_close
prompt_history_dump "$first" >"$root/snapshot"
terminal_start --resume "$first"
prompt_history_dump "$first" >"$root/prompts"; cmp "$root/prompts" "$root/snapshot"
send '\022history-steering\r'; expect_prompt '> history-steering'; [[ $(requests) == "$count" ]]
send '\025'; terminal_close
mkdir "$root/other"
base[${#base[@]}-1]="$root/other"
terminal_start -N
send '\020/goal\r'; expect 'No goal.'; [[ $(requests) == "$count" ]]
prompt_history_dump "$(current_session_id)" >"$root/prompts"
json "$root/prompts" '.prompt == "/goal"'
terminal_close
base[${#base[@]}-1]="$work"
unset XDG_STATE_HOME
terminal_start -N; send '/goal\r'; expect 'No goal.'
prompt_history_dump "$(current_session_id)" "$root/.local/state/cai" >"$root/prompts"
json "$root/prompts" '.prompt == "/goal"'
[[ ! -e "$root/.local/state/cai/history" && ! -e "$root/.local/state/softline" ]]
terminal_close
[[ -z $(find "$root/state" "$root/.local/state" -name '*.history' -print) ]]
# The flag overrides the environment; the recall window does not delete data.
export CAI_PROMPT_HISTORY_LIMIT=1
terminal_start -N --prompt-history-limit 2
limited=$(current_session_id)
send '/goal\r'; expect 'No goal.'
send '/status\r'; expect 'Metric'
send '\033[A'; expect_prompt '> /status'
send '\033[A'; expect_prompt '> /goal'
send '\033[A'; expect_prompt '> /goal'
send '\025'; terminal_close
prompt_history_dump "$limited" "$root/.local/state/cai" >"$root/prompts"
[[ $(wc -l <"$root/prompts") == 3 ]]
# Zero disables both recording and recall, including local commands.
terminal_start -N --prompt-history-limit 0
disabled=$(current_session_id)
send '/goal\r'; expect 'No goal.'
send '\033[A'; expect_prompt '> '
absent "$term/prompt" /goal
terminal_close
prompt_history_dump "$disabled" "$root/.local/state/cai" >"$root/prompts"
[[ ! -s "$root/prompts" && $(requests) == "$count" ]]
unset CAI_PROMPT_HISTORY_LIMIT
# Slash commands render Markdown, persist the conversation override, and keep
# stored-entry pruning explicitly separate from the current recall setting.
terminal_start --resume "$disabled"
send '/retention\r'; expect '30 days from last update'
send '/retention off \r'; expect 'forever'
send '/history\r'; expect '200 entries'; expect 'max_entries'
send '/history 2\r'; expect '2 entries'
send '/history bad\r'; expect 'use /history COUNT'
send '/retention -1\r'; expect 'use /retention DAYS'
send '/retention 7\r'; expect '7 days from last update'
terminal_close
[[ $("$history_reader" policy "$root/.local/state/cai" "$disabled") == 7 ]]
terminal_start --resume "$disabled" --conversation-retention-days 2
send '/retention\r'; expect '7 days from last update'
send '/retention 0\r'; expect 'forever'
terminal_close
[[ $("$history_reader" policy "$root/.local/state/cai" "$disabled") == 0 ]]
# A removed conversation is omitted; auto-resume uses an available one.
"$history_reader" drop "$root/.local/state/cai" "$disabled"
run -R; ok; absent "$root/stdout" "$disabled"
terminal_start
[[ $(current_session_id) != "$disabled" ]]
terminal_close
[[ $(requests) == "$count" ]]
