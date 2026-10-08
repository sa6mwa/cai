#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_TERMINAL_STDERR=1
export SHELL="$root/user shell" CAI_TEST_SHELL_LAUNCHES="$root/launches"
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
terminal_start -N
send '/shell\r'; expect SHELL_HANDOFF_READY; wait_for canonical 1
resize 48
send 'touch ../normal-permissions; printf UNSHARED_TERMINAL_OUTPUT; exit\r'
wait_for canonical 0
[[ -f "$root/normal-permissions" && $(requests) == 0 ]]
plan '{"plan":["POST HANDOFF ANSWER\n\nword word word word word word word word word word word word word word word word word word word word\n\n"]}'
send 'continue after the handoff\r'; expect 'POST HANDOFF ANSWER'; expect 'Worked for'
absent "$root/peer/requests.jsonl" UNSHARED_TERMINAL_OUTPUT
absent "$root/peer/requests.jsonl" '<user_shell_command>'
send '\022/shell\r'; expect '/shell'; canonical 0
[[ $(wc -c <"$root/launches") == 1 ]]
send '\r'; wait_for canonical 1
send 'exit 7\r'; expect 'interactive shell exited 7'; wait_for canonical 0
send '\025!sh\r'; wait_for canonical 1
send 'kill -KILL $$\r'; expect 'interactive shell terminated by signal 9'; wait_for canonical 0
plan '{"plan":[{"text":"BusyHandoffDone","pause":true}]}'
send 'busy turn\r'; expect 'STREAM in flight'
count=$(wc -c <"$root/launches")
send '/shell\r'; expect 'Shell is available only between turns'
send '\t\033\r'; expect 'Use Enter to open the shell'
[[ $(wc -c <"$root/launches") == "$count" ]]
send '\025'; release; expect BusyHandoffDone
terminal_close
export SHELL="$root/missing; touch injected"
terminal_start -N
send '/shell\r'; expect 'interactive shell exited 127'; [[ ! -e "$work/injected" ]]
terminal_close
for fallback in unset empty; do
  if [[ "$fallback" == unset ]]; then unset SHELL; else export SHELL=; fi
  terminal_start -N
  send '!sh\r'; wait_for canonical 1
  send 'touch fallback-shell; exit\r'; wait_for canonical 0; [[ -f "$work/fallback-shell" ]]
  rm "$work/fallback-shell"; terminal_close
done
for command in /shell '!sh'; do
  printf '%s\n/quit\n' "$command" | "${base[@]}" >"$root/stdout" 2>"$root/stderr"
  cat "$root/stdout" "$root/stderr" >"$root/combined"
  contains "$root/combined" 'interactive shell requires terminal input and output'
done
logs
jq -se 'any(.[]; .event == "terminal_shell_started") and any(.[]; .event == "terminal_shell_completed" and .exit_code == 7)' "$root/logs.jsonl" >/dev/null
