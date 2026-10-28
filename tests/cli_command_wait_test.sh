#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
scenario=${2:-process}
peer_start
base+=(--terminal)
printf '[client]\napi_protocol=%s\n[terminal]\nyield_time_ms=0\n' "$protocol" >"$root/wait.ini"
base+=(--config "$root/wait.ini")
chat=false; [[ "$protocol" != chat_completions ]] || chat=true
if [[ "$scenario" == process ]]; then
terminal_start -N
send '/yield_time_ms\r'; expect 'Foreground commands:'
send '/bg\r'; expect '10000 ms'
send '/yield_time_ms 250\r'; expect '250 ms'
send '/background\r'; expect '250 ms'
send '/fg\r'; expect 'Foreground commands:'
send '/background\r'; expect '10000 ms'
send '/yield_time_ms -1\r'; expect '0..30000'
send '/yield_time_ms\r'; expect '10000 ms'
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"exec_command",arguments:{cmd:"stty -echo; read line; printf COMMAND_FINISHED:%s \"$line\"",tty:true,yield_time_ms:0}},
  {tool:"exec_command",arguments:{cmd:"touch must-not-run",yield_time_ms:0}},
  {tool:"write_stdin",arguments:{session_id:"terminal-1",chars:"ok\n",yield_time_ms:1000}},
  {tool:"exec_command",arguments:{cmd:"test ! -t 0 && test ! -t 1 && test ! -t 2 && { read line || printf PIPE_EOF; }; printf PIPE_STDERR >&2",yield_time_ms:1000}},
  "COMMAND TURN COMPLETE"]}')"
prompt 'exercise the single process' 'COMMAND TURN COMPLETE'
contains "$term/text" '* Busy command not started'
contains "$term/text" 'Active handle:'
contains "$term/text" 'COMMAND_FINISHED:ok'
contains "$term/text" PIPE_EOF
contains "$term/text" PIPE_STDERR
absent "$term/text" '"accepted"'
absent "$term/text" '* Failed exec_command'
[[ ! -e "$work/must-not-run" ]]
id=$(current_session_id)
terminal_close
logs
jq -se '([.[]|select(.event=="terminal_command_started")][0].tool_call_id) as $origin |
  ([.[]|select(.event=="terminal_command_started")]|length)==2 and
  any(.[]; .event=="terminal_output" and (.msg|contains("COMMAND_FINISHED")) and .tool_call_id==$origin) and
  any(.[]; .event=="terminal_output" and (.msg|contains("PIPE_EOF")))' "$root/logs.jsonl" >/dev/null
before=$(requests)
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" '* Busy command not started'
contains "$term/text" 'COMMAND_FINISHED:ok'
absent "$term/text" '"accepted"'
[[ $(grep -Ec '\* Ran for' "$term/text") == 2 ]]
terminal_close
[[ $(requests) == "$before" ]]
exit 0
fi
[[ "$scenario" == foreground ]] || fail "Unknown scenario: $scenario"
# Foreground keeps inference blocked while allowing UI progress and Escape.
mkfifo "$work/gate"
printf 'printf FG_PROGRESS\nread x < gate\nprintf FG_DONE\n' >"$work/foreground.sh"
printf 'printf FG_CANCEL\nread x < gate\n' >"$work/cancel.sh"
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
 {tool:"exec_command",arguments:{cmd:"sh foreground.sh",yield_time_ms:0}},
 "FOREGROUND TURN COMPLETE"]}')"
terminal_start -N --foreground
before=$(requests)
send 'exercise foreground completion\r'; expect FG_PROGRESS
[[ $(requests) == $((before+1)) ]]
printf 'go\n' >"$work/gate"
expect 'FOREGROUND TURN COMPLETE'; expect 'Worked for'
contains "$term/text" FG_DONE
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
 {tool:"exec_command",arguments:{cmd:"sh cancel.sh",yield_time_ms:0}},
 "UNWANTED CONTINUATION"]}')"
send 'exercise foreground cancellation\r'; expect FG_CANCEL
send '\033'; expect 'Turn stopped.'
absent "$term/text" 'UNWANTED CONTINUATION'
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
 {tool:"exec_command",arguments:{cmd:"printf AFTER_CANCEL",yield_time_ms:0}},
 "RECOVERED TURN COMPLETE"]}')"
prompt 'continue after cancellation' 'RECOVERED TURN COMPLETE'
contains "$term/text" AFTER_CANCEL
terminal_close
