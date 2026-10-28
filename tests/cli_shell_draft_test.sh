#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_TERMINAL_STDERR=1
export SHELL="$root/draft shell" CAI_TEST_SHELL_LAUNCHES="$root/launches"
cat >"$SHELL" <<'SH'
#!/bin/sh
if [ "$1" != -i ]; then exec /bin/sh "$@"; fi
test -t 0 && test -t 1 && test -t 2 || exit 77
printf x >>"$CAI_TEST_SHELL_LAUNCHES"
printf 'DRAFT_SHELL_READY\n'
exec /bin/sh -i
SH
chmod 700 "$SHELL"
canonical() { [[ -s "$term/tty" && $(cut -d' ' -f1 "$term/tty") == "$1" ]]; }
terminal_start -N
send 'SHELL-STASH\033k'
# Preserve a multiline Unicode draft and its cursor across terminal resize.
send 'keep å€\012second tail\033[D\033[D\033[D\033[D\033s'
expect DRAFT_SHELL_READY; wait_for canonical 1
[[ $(requests) == 0 ]]
resize 64
send 'printf ALT_S_LOCAL_ONLY; stty size; exit\r'
expect 'Shell exited 0.'; wait_for canonical 0; expect '30 64'
send '\014' # Native Ctrl-L refresh must retain the suspended draft/cursor too.
plan '{"plan":["DRAFT PRESERVED"]}'
send 'INSERT \r'; expect_turn "$send_offset" 'DRAFT PRESERVED'
json "$root/peer/requests.jsonl" 'any(.. | objects; .text? == "keep å€\nsecond INSERT tail")'
absent "$root/peer/requests.jsonl" ALT_S_LOCAL_ONLY
absent "$root/peer/requests.jsonl" '<user_shell_command>'
send '\033j'; expect_prompt SHELL-STASH
send '\033n'
# Cursor movement starts a distinct edit group before X. Undo must survive a
# failed shell, not just a successful one, without submitting the draft.
send 'undo draft\033[D\033[CX\033s'; expect DRAFT_SHELL_READY; wait_for canonical 1
send 'exit 7\r'; expect 'interactive shell exited 7'; wait_for canonical 0
plan '{"plan":["UNDO PRESERVED"]}'
send '\037\r'; expect_turn "$send_offset" 'UNDO PRESERVED'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" 'any(.. | objects; .text? == "undo draft")'
# Busy shortcut attempts leave the editor and active turn intact.
plan '{"plan":[{"pause":true,"pause_text":"BUSY WAITING\n","text":"BUSY DONE"}]}'
send 'work\r'; expect BUSY
before=$(requests)
send 'retained busy\033s'; expect 'Shell is available only between turns.'
expect_prompt 'retained busy'
[[ $(wc -c <"$root/launches") == 2 && $(requests) == "$before" ]]
send '\025'; release; expect_turn "$send_offset" 'BUSY DONE'
terminal_close
# An executable failure must return to the same draft as well.
export SHELL="$root/missing-shell"
terminal_start -N
send 'failure draft\033s'; expect 'interactive shell exited 127'
wait_for canonical 0; expect_prompt 'failure draft'
[[ $(requests) == "$before" ]]
terminal_close
