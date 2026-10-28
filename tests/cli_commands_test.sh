#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
width=40
for colors in yes no; do
  if [[ "$colors" == no ]]; then export NO_COLOR=1; else unset NO_COLOR; fi
  terminal_start -N
  if [[ "$colors" == yes ]]; then
    send '/help\r'; expect manual.
    sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' "$term/feed" >"$root/help"
    for alias in /m /r /t; do grep -Eq "$alias([^a-zA-Z]|$)" "$root/help"; done
    for hint in Ctrl-Y Alt-Y Alt-K Alt-J Alt-N Ctrl-T Ctrl-G Ctrl-_ 'Bracketed' 'ten-entry' 'stash'; do contains "$root/help" "$hint"; done
    send 'YANK_DRAFT\025\031'; expect_prompt YANK_DRAFT
    send '\025'
    send '/t\r'; expect 'Current: default:default'; expect 'pairings'
  fi
  for command in '/m gpt-6-luna low' '/r medium' '/output full'; do
    offset=$(wc -c <"$term/raw")
    send "$command\r"; expect "\$ $command"
    case "$command" in
      /m*) expect 'Model changed' ;;
      /r*) expect 'Reasoning effort changed' ;;
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
  for command in '/r invalid' '/output invalid' '/resume 999' '/resume invalid' '/unknown' '/mm'; do
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
  if [[ "$colors" == yes ]]; then
    plan '{"plan":["QUIT TEST DONE"]}'
    prompt 'finish a turn before quitting' 'QUIT TEST DONE'
  fi
  # Both exit aliases replace the gray status message before releasing the
  # editor, without adding a farewell to the conversation feed.
  count=$(requests)
  if [[ "$colors" == yes ]]; then command=/quit; else command=/exit; fi
  terminal_close "$command"
  [[ $(requests) == "$count" ]]
  # The peer publishes its sticky-row snapshot before appending the matching
  # text frame. A pending Worked-for redraw can therefore arrive after send's
  # offset. Check from the applied quit command, not the input write.
  tail -c +$((send_offset+1)) "$term/text" |
    awk -v marker="\$ $command" 'index($0, marker) { applied=1 } applied' >"$root/farewell"
  contains "$root/farewell" "\$ $command"
  contains "$root/farewell" '! Goodbye.'
  absent "$root/farewell" 'Worked for'
  tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/farewell-feed"
  absent "$root/farewell-feed" 'Goodbye.'
done
