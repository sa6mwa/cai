#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base=("$cli" -p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-image-generation
  --config "$root/protocol.ini" -C "$work")
printf "printf '\\033[32mREPLAY01\\033[0m\\nREPLAY02\\nREPLAY03\\nREPLAY04\\nREPLAY05\\nREPLAY06\\n'\nexit 7\n" >"$work/replay-command.sh"
printf 'before\nold\nafter\n' >"$work/source.txt"
patch=$'*** Begin Patch\n*** Update File: source.txt\n@@\n-old\n+new\n*** End Patch\n'
bad_patch=$'*** Begin Patch\n*** Update File: source.txt\n@@\n-never existed\n+unused\n*** End Patch\n'
chat=false; [[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" --arg patch "$patch" --arg bad "$bad_patch" '{chat:$chat,plan:[
  {tool:"exec_command",arguments:{cmd:"sh replay-command.sh",yield_time_ms:1000}},
  {tool:"write_stdin",arguments:{session_id:"terminal-1",chars:"",yield_time_ms:1000}},
  {tool:"read_file",arguments:{path:"source.txt"}},
  {tool:"apply_patch",arguments:(if $chat then {input:$patch} else $patch end)},
  {tool:"apply_patch",arguments:(if $chat then {input:$bad} else $bad end)},"REPLAY FINAL ANSWER"]}')"
run -Nni 'REPLAY ORIGINAL PROMPT'; ok
[[ $(stdout_text) == 'REPLAY FINAL ANSWER' ]]
id=$(current_session_id)
[[ -n "$id" ]]
before=$(requests)
for mode in compact full; do
  terminal_start --resume "$id" --tool-output "$mode"
  expect 'Conversation resumed.'
  expect 'REPLAY FINAL ANSWER'
  contains "$term/text" 'REPLAY ORIGINAL PROMPT'
  contains "$term/text" 'REPLAY FINAL ANSWER'
  [[ $(grep -Ec '\* Ran for [0-9hms]+ sh replay-command.sh' "$term/text") == 1 ]]
  contains "$term/text" 'REPLAY01'; contains "$term/text" 'REPLAY05'
  contains "$term/text" 'exit=7 signal=0'
  contains "$term/text" '* Read source.txt'
  contains "$term/text" '* Edited source.txt'
  contains "$term/text" '1  before'; contains "$term/text" '3  after'
  contains "$term/text" '2 -old'; contains "$term/text" '2 +new'
  contains "$term/text" '* Failed apply_patch'
  contains "$term/text" 'source.txt'
  for unwanted in '* Running ' 'Tool completed:' '"session_id"' '\x1b'; do
    absent "$term/text" "$unwanted"
  done
  if [[ "$mode" == compact ]]; then
    absent "$term/text" REPLAY06; contains "$term/text" 'output omitted'
  else
    contains "$term/text" REPLAY06; absent "$term/text" 'output omitted'
  fi
  terminal_close
done
[[ $(requests) == "$before" ]]
[[ $(cat "$work/source.txt") == $'before\nnew\nafter' ]]
