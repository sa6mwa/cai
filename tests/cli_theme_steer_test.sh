#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
terminal_start -N
for selection in gruvbox:rose-pine-dawn no-such-theme plain queued parent-offset child-offset review-offset; do
  theme=$selection
  command=/theme
  intent=S key='\033\r'
  if [[ "$selection" == queued ]]; then theme=dracula; intent=Q; key='\r'; fi
  if [[ "$selection" == queued || "$selection" == gruvbox:rose-pine-dawn ]]; then command=/t; fi
  case "$selection" in
    parent-offset) command=/theme-offset; theme=-300 ;;
    child-offset) command=/subagent-theme-offset; theme=1234 ;;
    review-offset) command=/review-theme-offset; theme=-1 ;;
  esac
  steer="THEME_FOLLOWING_STEER_$selection"
  before=$(requests)
  plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
    {pause:true,pause_text:"ThemeBoundaryBegin\n\n",tool:"get_goal",arguments:{},text:"ThemeBoundaryEnd"},"ThemeSteerContinued"
  ]}')"
  send "Start theme seam $theme\r"; expect ThemeBoundaryBegin
  send "$command $theme$key"; expect "$intent 1. $command $theme"
  send "$steer\033\r"; expect "S 2. $steer"
  absent "$term/feed" "> $steer"
  release; expect_assistant ThemeSteerContinued; expect 'Worked for'
  # The very next inference within this turn must contain the steer. An
  # eventual new turn after the theme drains does not satisfy this check.
  sed -n "$((before+2))p" "$root/peer/requests.jsonl" >"$root/continuation"
  contains "$root/continuation" "$steer"
  absent "$root/continuation" "$command $theme"
  [[ $(requests) == $((before+2)) ]]
  if [[ "$theme" == no-such-theme ]]; then
    contains "$term/feed" 'unknown softline theme'
  elif [[ "$command" == *-offset ]]; then
    contains "$term/feed" "theme offset: $theme."
  else
    contains "$term/feed" 'Theme changed to'
  fi
  absent "$term/feed" 'Change themes between turns'
done
terminal_close
