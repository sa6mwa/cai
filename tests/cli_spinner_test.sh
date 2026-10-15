#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_API_BASE_URL="$endpoint" CAI_TEST_SPINNER_TRACE="$root/spinner"
printf 'fixture file content\n' >"$work/data.txt"
printf 'busy 1\nspinner 1\n' >"$root/active"
printf 'busy 1\nspinner 1\nbusy 0\nspinner 0\n' >"$root/done"
for mode in tools provider_error cancel; do
  : >"$root/spinner"
  case "$mode" in
    tools)
      plan '{"plan":[{"pause":true,"tool":"read_file","arguments":{"path":"data.txt"}},{"tool":"read_file","arguments":{"path":"missing.txt"}},"TOOLS FINISHED"]}' ;;
    provider_error)
      plan '{"plan":[{"pause":true,"error":"fixture spinner provider error"}]}' ;;
    cancel)
      plan '{"plan":[{"pause":true,"text":"CANCELLED TAIL"}]}' ;;
  esac
  terminal_start -N
  # Idle startup and slash commands must not reset the animation either.
  send '/status\r'; expect 'Reasoning effort'; [[ ! -s "$root/spinner" ]]
  send "exercise spinner $mode\r"; expect 'STREAM in flight'
  # Wait for two real timer ticks; then trigger another status refresh via a
  # command during the same turn. Neither may restart softline's spinner.
  expect 'Working (0m 2s)'
  send '/status\r'; expect 'Reasoning effort'
  cmp "$root/active" "$root/spinner"
  if [[ "$mode" == cancel ]]; then
    send '\033'; expect 'Worked for'; release
    absent "$term/text" 'CANCELLED TAIL'
  else
    release
    if [[ "$mode" == tools ]]; then
      expect 'TOOLS FINISHED'
    else
      expect 'fixture spinner provider error'
    fi
    expect 'Worked for'
  fi
  cmp "$root/done" "$root/spinner"
  # A subsequent turn restarts exactly once and stops exactly once.
  plan '{"plan":["RECOVERED SPINNER"]}'
  prompt 'another turn' 'RECOVERED SPINNER'
  cat "$root/done" "$root/done" >"$root/two-turns"
  cmp "$root/two-turns" "$root/spinner"
  terminal_close
done
