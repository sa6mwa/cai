#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
history="$root/state/cai/history/$(printf %s "$work" | sha256sum | cut -d' ' -f1).history"
terminal_start -N
plan '{"plan":[{"text":"HistoryFirstDone","pause":true},"HistorySteerDone"]}'
offset=$(wc -c <"$term/text")
send '\033[200~history unicode λ\nsecond \\ tab\t\033[201~\r'; expect 'STREAM in flight'
printf 'history unicode λ\\nsecond \\\\ tab\\t\n' >"$root/expected"
cmp "$history" "$root/expected"
[[ $(stat -c %a "$history") == 600 && $(stat -c %a "$(dirname "$history")") == 700 ]]
[[ ! -e "$root/state/softline" ]]
send 'history-steering\033\r'; wait_for has "$history" history-steering
release; expect_turn "$offset" HistorySteerDone
[[ $(grep -c '^history-steering$' "$history") == 1 ]]
send '/new\r'; expect 'New session'; expect 'ID:'; expect 'Workspace:'; expect '$ /new'
absent "$term/feed" '--- session ---'
count=$(requests); cp "$history" "$root/snapshot"
send '\022history-steering\r'; expect_prompt '> history-steering'
[[ $(requests) == "$count" ]]; cmp "$history" "$root/snapshot"
send '\025'; terminal_close
cp "$history" "$root/snapshot"
terminal_start; cmp "$history" "$root/snapshot"
send '\022history-steering\r'; expect_prompt '> history-steering'; [[ $(requests) == "$count" ]]
send '\025'; terminal_close
mkdir "$root/other"
base[${#base[@]}-1]="$root/other"
terminal_start -N
send '\020/goal\r'; expect 'No goal.'; [[ $(requests) == "$count" ]]
other="$root/state/cai/history/$(printf %s "$root/other"|sha256sum|cut -d' ' -f1).history"
[[ $(cat "$other") == /goal ]]
terminal_close
base[${#base[@]}-1]="$work"
printf unfinished-tail >>"$history"
terminal_start -N; send '/goal\r'; expect 'No goal.'; absent "$history" unfinished-tail
terminal_close
mv "$history" "$history.original"; ln -s "$history.original" "$history"
# Unsafe history storage must abort before an unsaved prompt is submitted.
terminal_launch -N
wait_for test -s "$term/status"
if wait "$terminal_pid"; then fail 'Unsafe history was accepted'; fi
terminal_pid=
cat "$term/text" "$term/stderr" >"$root/combined"
contains "$root/combined" history
exec 7>&-
[[ $(requests) == "$count" ]]
rm "$history"; mv "$history.original" "$history"
unset XDG_STATE_HOME
terminal_start -N; send '/goal\r'; expect 'No goal.'
[[ $(cat "$root/.local/state/cai/history/$(basename "$history")") == /goal ]]
[[ ! -e "$root/.local/state/softline" ]]
terminal_close
