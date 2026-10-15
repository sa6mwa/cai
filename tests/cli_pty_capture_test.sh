#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export SHELL="$root/capture-shell"
cat >"$SHELL" <<'SH'
#!/bin/sh
if [ "$1" != -i ]; then exec /bin/sh "$@"; fi
test -t 0 && test -t 1 && test -t 2 || exit 77
stty -echo
printf 'CAPTURE_PTY_READY\n'
exec /bin/sh -i
SH
chmod 700 "$SHELL"
config="$root/capture.ini"
printf '[runtime]\nshell_context_max_lines = 3\nshell_context_max_bytes = 4096\n' >"$config"
base+=(--config "$config")
terminal_start -N
send '/shell\r'; expect CAPTURE_PTY_READY
send 'printf "DISCARDED_HEAD\\nkeep-a\\n\\033[31mkeep-b\\033[0m\\nkeep-c\\n"; exit\r'
expect 'Shell exited 0.'
[[ $(requests) == 0 ]]
plan '{"plan":["CaptureContextDone"]}'
prompt 'inspect shell context' CaptureContextDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'keep-a'
contains "$root/request" 'keep-b'
contains "$root/request" 'keep-c'
contains "$root/request" 'Earlier output truncated'
absent "$root/request" DISCARDED_HEAD
jq -e '(.input // .messages) | tostring | contains("\u001b") | not' "$root/request" >/dev/null
send '/shell-context off\r'; expect 'off'
send '!sh\r'; expect CAPTURE_PTY_READY
send 'printf "PRIVATE_PTY_RESULT\\n"; exit\r'; expect 'Shell exited 0.'
plan '{"plan":["PrivateContextDone"]}'
prompt 'inspect private shell setting' PrivateContextDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" PRIVATE_PTY_RESULT
terminal_close
# Restart/replay must preserve the shared text without executing a shell again.
plan '{"plan":["ReplayContextDone"]}'
terminal_start
prompt 'inspect persisted shell context' ReplayContextDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" keep-c
absent "$root/request" PRIVATE_PTY_RESULT
terminal_close
# Environment narrows INI retention; the explicit CLI value wins over both.
export CAI_SHELL_CONTEXT_MAX_LINES=1 CAI_SHELL_CONTEXT=false
terminal_start -N --shell-context --shell-context-max-lines 2
send '!sh\r'; expect CAPTURE_PTY_READY
send 'printf "ENV_OMITTED\\nCLI_KEPT_ONE\\nCLI_KEPT_TWO\\n"; exit\r'; expect 'Shell exited 0.'
plan '{"plan":["PrecedenceDone"]}'
prompt 'inspect configured retention' PrecedenceDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" ENV_OMITTED
contains "$root/request" CLI_KEPT_ONE
contains "$root/request" CLI_KEPT_TWO
terminal_close
# Exercise the real default tail beyond the former 32 KiB runtime cap, then
# reopen the encrypted store to verify that the larger completion is resumable.
terminal_start -N --shell-context --shell-context-max-lines 5000 --shell-context-max-bytes 4194304
send '!sh\r'; expect CAPTURE_PTY_READY
send 'i=0; while [ "$i" -lt 5100 ]; do printf "TAIL_%04d\\n" "$i"; i=$((i+1)); done; exit\r'
expect 'Shell exited 0.'
plan '{"plan":["LargeTailDone"]}'
prompt 'inspect large tail' LargeTailDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" TAIL_0099
contains "$root/request" TAIL_0100
contains "$root/request" TAIL_5099
terminal_close
plan '{"plan":["LargeTailReplayDone"]}'
terminal_start --shell-context
prompt 'inspect large tail after replay' LargeTailReplayDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" TAIL_0100
contains "$root/request" TAIL_5099
terminal_close
