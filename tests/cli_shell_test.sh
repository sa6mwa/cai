#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# Force the truncation case below independently of the normal 4 MiB default.
terminal_start -N --shell-context-max-bytes 32768
send '/goal status\r'; expect 'No goal.'; contains "$term/text" '$ /goal status'
send '/help\r'; expect 'Run cai --help'; contains "$term/text" '$ /help'
send '/nonesuch\r'; expect 'Unknown command'; [[ $(requests) == 0 ]]
send '!test ! -t 0 && test ! -t 1 && test ! -t 2 && cat && printf "# literal **stars** & <tags>\\n" && printf "stderr-marker\\n" >&2; touch ../normal-user-permissions\r'
expect 'Shell command exited 0'; contains "$term/text" '# literal **stars** & <tags>'
contains "$term/text" stderr-marker; [[ -f "$root/normal-user-permissions" && $(requests) == 0 ]]
plan '{"plan":["SHELL RESULT USED"]}'
prompt 'use the shell result' 'SHELL RESULT USED'
contains "$root/peer/requests.jsonl" '<user_shell_command>'
contains "$root/peer/requests.jsonl" 'Exit code: 0'
absent "$root/peer/requests.jsonl" '/goal status'; absent "$root/peer/requests.jsonl" '/help'
send '/shell-context off\r'; expect 'stay out of model context'
send '!printf secret-local-result\r'; expect secret-local-result
plan '{"plan":["LOCAL ONLY DONE"]}'
prompt 'verify local-only command' 'LOCAL ONLY DONE'
absent "$root/peer/requests.jsonl" secret-local-result
send '/shell-context on\r'; expect 'shared with the model'
send '!printf nonzero-error >&2; exit 7\r'; expect 'Shell command exited 7'
plan '{"plan":["FAILURE EXPLAINED"]}'
prompt 'explain the command failure' 'FAILURE EXPLAINED'
contains "$root/peer/requests.jsonl" 'Exit code: 7'; contains "$root/peer/requests.jsonl" nonzero-error
send '!printf EARLY-shell; while [ ! -f gate ]; do sleep .01; done; printf LATE-shell\r'; expect EARLY-shell
count=$(requests)
send '/shell-context off\r'; expect 'stay out of model context'
queued_offset=$(wc -c <"$term/text")
send 'queued behind the shell\r'; expect 'queued behind the shell'; [[ $(requests) == "$count" ]]
send '!touch never-started\r'; expect 'a shell command is running'; [[ ! -e "$work/never-started" ]]
plan '{"plan":["QUEUED DONE"]}'
touch "$work/gate"; expect_turn "$queued_offset" 'QUEUED DONE'
contains "$root/peer/requests.jsonl" EARLY-shellLATE-shell
send '/shell-context on\r'; expect 'shared with the model'
send '!printf CANCELLABLE; sleep 60\r'; expect CANCELLABLE
send '\003'; expect 'Shell command exited 137'
send '!kill -TERM $$\r'; expect 'Shell command exited 143'
send '!\r'; expect 'shell command must contain'
send '!printf "\\000\\033\\377\\342\\202\\254"\r'; expect 'Shell command exited 0'; contains "$term/text" '\x00\x1b\xff€'
send '!printf replay-once >> executions; cat executions\r'; expect 'Shell command exited 0'; contains "$term/text" replay-once
# Large output comes from a shell data producer, not a Python test driver.
send '!awk "BEGIN {for (i=0;i<70000;i++) printf \"L\"; print \"\\nRAW-END\"}"\r'; expect 'Shell command exited 0'; contains "$term/text" RAW-END
plan '{"plan":["BOUNDED DONE"]}'
prompt 'bounded shell context' 'BOUNDED DONE'
contains "$root/peer/requests.jsonl" 'Earlier output truncated;'
terminal_close
logs
jq -se 'any(.[]; .msg == "cai.user_shell.completed" and (.duration_ms|type) == "number" and (.bytes|type) == "number" and .role == "user")' "$root/logs.jsonl" >/dev/null
absent "$term/text" 'invalid kvfmt'
count=$(requests)
terminal_start; expect RAW-END; expect secret-local-result
[[ $(cat "$work/executions") == replay-once && $(requests) == "$count" ]]
terminal_close
export SHELL="$root/missing-shell"
terminal_start -N --no-shell-context
send '!echo unavailable\r'; expect 'Shell command exited 127'; expect 'Cannot execute user shell'
plan '{"plan":["FAILED UNSHARED DONE"]}'
prompt 'after failed unshared shell' 'FAILED UNSHARED DONE'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" '<user_shell_command>'
terminal_close
