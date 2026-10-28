#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_TERMINAL_STDERR=1
export SHELL="$root/user shell" CAI_TEST_SHELL_LAUNCHES="$root/launches"
export CAI_TEST_BACKGROUND_PID="$root/background-pid"
cat >"$SHELL" <<'SH'
#!/bin/sh
if [ "$1" != -i ]; then exec /bin/sh "$@"; fi
test -t 0 && test -t 1 || exit 77
printf x >>"$CAI_TEST_SHELL_LAUNCHES"
printf 'SHELL_HANDOFF_READY\n'
exec /bin/sh -i
SH
chmod 700 "$SHELL"
canonical() { [[ -s "$term/tty" && $(cut -d' ' -f1 "$term/tty") == "$1" ]]; }
shell_ready() {
  local flags foreground application
  [[ -s "$term/tty" ]] || return 1
  read -r flags foreground application <"$term/tty"
  [[ "$flags" == 1 && "$foreground" -gt 0 && "$foreground" != "$application" ]]
}
terminal_start -N
send '/shell\r'; expect SHELL_HANDOFF_READY; wait_for shell_ready
resize 48
send 'stty size; touch ../normal-permissions; printf SHARED_TERMINAL_OUTPUT; exit\r'
expect 'Shell exited 0.'; wait_for canonical 0
expect '30 48'
[[ -f "$root/normal-permissions" && $(requests) == 0 ]]
plan '{"plan":["POST HANDOFF ANSWER\n\nword word word word word word word word word word word word word word word word word word word word\n\n"]}'
send 'continue after the handoff\r'; expect 'POST HANDOFF ANSWER'; expect 'Worked for'
absent "$root/peer/requests.jsonl" SHARED_TERMINAL_OUTPUT
absent "$root/peer/requests.jsonl" '<user_shell_command>'
# Ctrl-C reaches the foreground job on the terminal and leaves its shell
# usable; it does not cancel CAI or leak a sleeping command.
send '!sh\r'; expect SHELL_HANDOFF_READY
send 'printf INTERRUPT_READY; sleep 60\r'; expect INTERRUPT_READY
send '\003'; expect '$'
send 'printf AFTER_INTERRUPT; exit\r'; expect 'Shell exited 0.'
send '\022/shell\r'; expect '/shell'; wait_for canonical 0
[[ $(wc -c <"$root/launches") == 2 ]]
send '\r'; expect SHELL_HANDOFF_READY; wait_for shell_ready
send 'exit 7\r'; expect 'interactive shell exited 7'; wait_for canonical 0
send '\025!sh\r'; expect SHELL_HANDOFF_READY; wait_for shell_ready
send 'kill -KILL $$\r'; expect 'interactive shell terminated by signal 9'; wait_for canonical 0
send '!sh\r'; expect SHELL_HANDOFF_READY
send 'exit 137\r'; expect 'interactive shell exited 137'
plan '{"plan":[{"text":"BusyHandoffDone","pause":true}]}'
send 'busy turn\r'; expect 'STREAM in flight'
count=$(wc -c <"$root/launches")
send '/shell\r'; expect 'Q 1. /shell'
send '\033e'; expect_prompt /shell
send '\025/shellshare\r'; expect 'Q 1. /shellshare'
send '\033e'; expect_prompt /shellshare
send '\025!sh+\t'; expect_prompt '!sh+'
send '\r'; expect 'Q 1. !sh+'
send '\033\r'; expect 'S 1. !sh+'
[[ $(wc -c <"$root/launches") == "$count" ]]
release; expect BusyHandoffDone
expect SHELL_HANDOFF_READY # Shared handoff owns a child PTY, not the outer TTY.
send 'exit\r'; expect 'Shell exited 0.'; wait_for canonical 0
send '!sh\r'; expect SHELL_HANDOFF_READY
send 'sleep 60 & echo $! > "$CAI_TEST_BACKGROUND_PID"; exit\r'
expect 'Shell exited 0.'
if [[ -s "$root/background-pid" ]]; then kill "$(cat "$root/background-pid")" 2>/dev/null || true; fi
terminal_close
export SHELL="$root/missing; touch injected"
terminal_start -N
send '/shell\r'; expect 'interactive shell exited 127'; [[ ! -e "$work/injected" ]]
terminal_close
for fallback in unset empty; do
  if [[ "$fallback" == unset ]]; then unset SHELL; else export SHELL=; fi
  terminal_start -N
  send '!sh\r'; wait_for shell_ready
  send 'touch fallback-shell; exit\r'; expect 'Shell exited 0.'; wait_for canonical 0; [[ -f "$work/fallback-shell" ]]
  rm "$work/fallback-shell"; terminal_close
done
for command in /shell '!sh' /shellshare '!sh+'; do
  printf '%s\n/quit\n' "$command" | "${base[@]}" >"$root/stdout" 2>"$root/stderr"
  cat "$root/stdout" "$root/stderr" >"$root/combined"
  contains "$root/combined" 'interactive shell requires terminal input and output'
done
logs
jq -se 'any(.[]; .event == "terminal_shell_started") and any(.[]; .event == "terminal_shell_completed" and .exit_code == 7)' "$root/logs.jsonl" >/dev/null
