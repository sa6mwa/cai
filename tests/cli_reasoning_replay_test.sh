#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
if [[ "${2:-}" == host ]]; then
  export CAI_TEST_APP_REPLAY_PATH="$root/replay-events"
fi
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false; [[ "$protocol" != chat_completions ]] || chat=true
printf 'fixture input\n' >"$work/input.txt"
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
 {reasoning:["**Older heading**\n\nOLDER reasoning body."],text:"OLDER ANSWER"}]}')"
terminal_start -N
prompt 'older task' 'OLDER ANSWER'
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[
 {reasoning:["**Title only**"],tool:"read_file",arguments:{path:"input.txt"}},
 {preamble:"COMMENTARY before summary.",
  reasoning:["**Current heading**\r","\n\nCURRENT **body** λ ","continues.\n\nCURRENT second paragraph."],
  tool:"read_file",arguments:{path:"input.txt"}},
 {reasoning:["**Final heading**\n\nFINAL reasoning body."],text:"LATEST ANSWER"}]}')"
prompt 'latest task' 'LATEST ANSWER'
for line in '  > CURRENT body λ continues.' '  > CURRENT second paragraph.' '  > FINAL reasoning body.'; do
  contains "$term/feed" "$line"
done
terminal_close
id=$(current_session_id)
run --export "$id"; ok
jsonl=$(head -1 "$root/stdout")
jq -se '[.[]|select(.record_type=="event" and .type=="reasoning_summary_started")]|length==4' "$jsonl" >/dev/null
jq -se '[.[]|select(.record_type=="event" and .type=="reasoning_summary")|.data]|join("")|
 contains("**Current heading**\r\n\nCURRENT **body** λ continues.")' "$jsonl" >/dev/null
before=$(requests)
for limit in 1 -1 0; do
  terminal_start --resume "$id" --replay-turns "$limit"
  expect 'Conversation resumed.'
  # Startup replay precedes Softline's live scroll region, so inspect the
  # terminal transcript rather than the peer's live-feed-only capture.
  if [[ "$limit" == 0 ]]; then
    absent "$term/text" 'CURRENT body'
    absent "$term/text" 'LATEST ANSWER'
  else
    contains "$term/text" 'COMMENTARY before summary.'
    contains "$term/text" '  > CURRENT body λ continues.'
    contains "$term/text" '  > CURRENT second paragraph.'
    contains "$term/text" $'  > FINAL reasoning body.\n\n  LATEST ANSWER'
    absent "$term/text" 'Title only'
    absent "$term/text" 'Current heading'
    absent "$term/text" 'Final heading'
    if [[ "$limit" == 1 ]]; then
      absent "$term/text" 'OLDER reasoning body.'
      absent "$term/text" 'OLDER ANSWER'
    else
      contains "$term/text" '  > OLDER reasoning body.'
      contains "$term/text" 'OLDER ANSWER'
    fi
  fi
  terminal_close
done
[[ $(requests) == "$before" ]]
if [[ "${2:-}" == host ]]; then
  # Disabling console replay must not hide observations from an embedding
  # host's C/Lua receiver callback.
  [[ $(grep -c ' reasoning_summary_started$' "$root/replay-events") == 4 ]]
  grep -q ' reasoning_summary$' "$root/replay-events"
  grep -q ' assistant_text_delta$' "$root/replay-events"
fi
