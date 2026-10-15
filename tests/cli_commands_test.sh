#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
width=40
for colors in yes no; do
  if [[ "$colors" == no ]]; then export NO_COLOR=1; else unset NO_COLOR; fi
  terminal_start -N
  for command in '/model gpt-6-luna low' '/reasoning medium' '/output full'; do
    offset=$(wc -c <"$term/raw")
    send "$command\r"; expect "\$ $command"
    case "$command" in
      /model*) expect 'Model changed' ;;
      /reasoning*) expect 'Reasoning effort changed' ;;
      /output*) expect 'setting applies' ;;
    esac
    tail -c +$((offset+1)) "$term/raw" >"$root/command"
    if [[ "$colors" == yes ]]; then
      contains "$root/command" $'\033[0;92m$\033[0m \033[38;2;215;160;95m'
    else
      absent "$root/command" $'\033[0;92m$'
      absent "$root/command" $'\033[38;2;215;160;95m'
    fi
    absent "$root/command" $'\033[3m'
  done
  for command in '/reasoning invalid' '/output invalid' '/resume 999' '/resume invalid' '/unknown'; do
    offset=$(wc -c <"$term/raw")
    send "$command\r"; expect "\$ $command"; expect '* ERROR:'
    tail -c +$((offset+1)) "$term/raw" >"$root/command"
    if [[ "$colors" == yes ]]; then
      contains "$root/command" $'\033[0;91m$\033[0m \033[38;2;215;160;95m'
    else
      absent "$root/command" $'\033[0;91m$'
    fi
  done
  # An invalid command containing Markdown punctuation remains literal, wraps,
  # and is echoed before its error rather than after the result.
  send '/unknown **literal** _path_ `code` [label](url)\r'
  expect '* ERROR:'
  contains "$term/feed" '**literal**'
  contains "$term/feed" '`code`'
  terminal_close
done
