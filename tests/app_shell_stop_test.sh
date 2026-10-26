#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_APP_CONTROL=shell_stop CAI_TEST_TERMINAL_STDERR=1
export CAI_TEST_APP_SHELL_SIGNAL="$root/shell-signal"
export CAI_TEST_APP_SHELL_QUEUED="$root/control-queued"
export CAI_TEST_APP_RELEASE="$root/control-ack"
export CAI_TEST_APP_SHELL_TTY="$root/shell-tty"
export SHELL="$root/interactive-shell"
mkfifo "$CAI_TEST_APP_SHELL_SIGNAL"
cat >"$SHELL" <<'SH'
#!/bin/sh
tty >"$CAI_TEST_APP_SHELL_TTY"
exec 8>"$CAI_TEST_APP_SHELL_SIGNAL"
printf R >&8
printf 'HOST_SHELL_READY\n'
exec /bin/sh -i
SH
chmod 700 "$SHELL"
for command in /shell '!sh' /shellshare '!sh+' alt-s; do
  case "$command" in *share|*+) export CAI_TEST_APP_SHELL_SHARED=1 ;; *) export CAI_TEST_APP_SHELL_SHARED=0 ;; esac
  rm -f "$CAI_TEST_APP_SHELL_QUEUED" "$CAI_TEST_APP_RELEASE"
  terminal_start -N
  if [[ "$command" == alt-s ]]; then send 'retained draft\033s'; else send "$command\r"; fi
  expect HOST_SHELL_READY
  wait_for test -f "$CAI_TEST_APP_SHELL_QUEUED"
  # Non-STOP controls stay queued during handoff. The shell remains usable.
  send "printf 'SHELL_%s\\n' STILL_USABLE\r"; expect SHELL_STILL_USABLE
  [[ ! -e "$CAI_TEST_APP_RELEASE" ]]
  # STOP must terminate the shell without sending exit or EOF to its PTY.
  case "$command" in
    '!sh'|'!sh+') send "sh -c 'printf S >&8; exec sleep 60'\r" ;;
    *) send 'printf S >&8\r' ;;
  esac
  wait_for test -s "$term/status"
  wait "$terminal_pid"; terminal_pid=
  [[ $(cat "$term/status") == 0 && $(requests) == 0 ]]
  [[ -e "$CAI_TEST_APP_RELEASE" ]]
  absent "$term/text" 'interactive shell exited 127'
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD
done
