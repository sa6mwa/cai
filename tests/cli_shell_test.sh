#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# Force the truncation case below independently of the normal 4 MiB default.
terminal_start -N --shell-context-max-bytes 32768
send '/goal status\r'; expect 'No goal.'; contains "$term/text" '$ /goal status'
send '/help\r'; expect 'startup options'; contains "$term/text" '$ /help'
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
send '!touch never-started\r'; expect 'Q 2. !touch never-started'
send '\033e'; expect_prompt '!touch never-started'
send '\025/output compact\r'; expect 'applies from the next tool call'
[[ ! -e "$work/never-started" ]]
plan '{"plan":["QUEUED DONE"]}'
touch "$work/gate"; expect_turn "$queued_offset" 'QUEUED DONE'
contains "$root/peer/requests.jsonl" EARLY-shellLATE-shell
send '/shell-context on\r'; expect 'shared with the model'
send '!printf CANCELLABLE; sleep 60\r'; expect CANCELLABLE
send '\003'; expect 'Shell command exited 137'
send '!kill -TERM $$\r'; expect 'Shell command exited 143'
send '!\r'; expect 'shell command must contain'
send '!printf "\\000\\033\\377\\342\\202\\254"\r'; expect 'Shell command exited 0'; contains "$term/text" '\x00\x1b\xff€'
send '!printf "C1-live: \\302\\200\\302\\205\\302\\233\\302\\237 UTF8: \\302\\240\\303\\234\\342\\202\\254\\360\\237\\233\\253\\n"\r'
expect 'Shell command exited 0'
contains "$term/feed" 'C1-live: \xc2\x80\xc2\x85\xc2\x9b\xc2\x9f UTF8:  Ü€🛫'
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
expect 'C1-live:'; expect 'Ü€🛫'
[[ $(cat "$work/executions") == replay-once && $(requests) == "$count" ]]
terminal_close
# Exercise raw saved output too: current shell tails are sanitized before
# storage, but imported/native-host records can contain encoded C1 controls.
# The hex form splits one scalar across events; the tail form flushes at once.
cat >"$root/c1-replay.jsonl" <<'JSON'
{"record_type":"checkpoint","checkpoint_created_at_ns":1,"applied_event_sequence":7,"state":{"version":1}}
{"record_type":"event","sequence":1,"type":"user_shell_started","data":"touch c1-replay-must-not-run"}
{"record_type":"event","sequence":2,"type":"user_shell_output_hex","data":"5245504c41592d43313a20c2"}
{"record_type":"event","sequence":3,"type":"user_shell_output_hex","data":"9b0a"}
{"record_type":"event","sequence":4,"type":"user_shell_completed","data":"0"}
{"record_type":"event","sequence":5,"type":"user_shell_started","data":"touch c1-tail-must-not-run"}
{"record_type":"event","sequence":6,"type":"user_shell_output_tail","data":"REPLAY-TAIL: \u0080\u009f Ü€🛫\n"}
{"record_type":"event","sequence":7,"type":"user_shell_completed","data":"0"}
JSON
run --import "$root/c1-replay.jsonl"; ok
imported=$(stdout_text)
terminal_start --resume "$imported"
expect 'REPLAY-C1: \xc2\x9b'
expect 'REPLAY-TAIL: \xc2\x80\xc2\x9f Ü€🛫'
[[ ! -e "$work/c1-replay-must-not-run" && ! -e "$work/c1-tail-must-not-run" && $(requests) == "$count" ]]
terminal_close
export SHELL="$root/missing-shell"
terminal_start -N --no-shell-context
send '!echo unavailable\r'; expect 'Shell command exited 127'; expect 'Cannot execute user shell'
plan '{"plan":["FAILED UNSHARED DONE"]}'
prompt 'after failed unshared shell' 'FAILED UNSHARED DONE'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" '<user_shell_command>'
terminal_close
