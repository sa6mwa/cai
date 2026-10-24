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
  # Informational replies use Markdown, including the code spans in usage
  # text. Both styled and NO_COLOR output must consume Markdown delimiters.
  offset=$(wc -c <"$term/raw")
  send '/output\r'; expect 'Tool output: full.'; expect '/output full.'
  tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/info"
  absent "$root/info" '**full**'; absent "$root/info" '`/output compact`'
  tail -c +$((offset+1)) "$term/raw" >"$root/styled-info"
  if [[ "$colors" == yes ]]; then
    grep -Eq $'\033\\[[0-9;]*[1-9][0-9;]*m/output compact\033\\[0m' "$root/styled-info"
  else
    if grep -Eq $'\033\\[[0-9;]*[1-9][0-9;]*m/output compact\033\\[0m' "$root/styled-info"; then
      fail 'NO_COLOR usage unexpectedly has a styled code span'
    fi
  fi
  send '/goal\r'; expect 'No goal.'; expect '/goal new TEXT'
  tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/info"
  absent "$root/info" '`/goal new TEXT`'; absent "$root/info" '**No goal.**'
  send '/shell-context invalid\r'; expect 'Usage: /shell-context [on|off]'
  tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/info"
  absent "$root/info" '`/shell-context'
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
  # Model identifiers are literal code, even when they contain Markdown.
  send '/model odd_`literal`**name**\r'; expect 'Model changed to odd_`literal`**name**'
  terminal_close
done
