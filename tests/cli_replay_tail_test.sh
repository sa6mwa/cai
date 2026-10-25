#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
scope=${1:-full}
export LOG_LEVEL=info
peer_start
config="$root/config/cai/config.ini"
for value in '' -2 junk 1.5 999999999999999999999999; do
  run --replay-turns "$value" --list; bad
done
run --replay-turns; bad
plan '{"plan":["ANSWER00 PARTA PARTB"]}'
run -Nni PROMPT00; ok
run --list; ok
original=$(awk 'NR == 1 {print $1}' "$root/stdout")
if [[ "$scope" == host ]]; then
  export CAI_TEST_APP_REPLAY_PATH="$root/replay-events"
  export CAI_TEST_APP_REPLAY_TURNS=0
  terminal_start --resume "$original"
  absent "$term/text" PROMPT00; absent "$term/text" ANSWER00
  terminal_close
  [[ $(grep -c ' assistant_text_delta$' "$root/replay-events") == 1 ]]
  contains "$root/replay-events" turn_submitted
  terminal_start --resume "$original" --replay-turns 1
  expect ANSWER00; expect PARTB
  terminal_close
  [[ $(grep -c ' assistant_text_delta$' "$root/replay-events") == 1 ]]
  exit 0
fi
run --export "$original"; ok
seed="$root/data/cai/exports/$original/$original.jsonl"
# Real checkpoint/provider item templates, synthetic completed journal turns.
# No inference loop or delays are needed to construct a long conversation.
jq -cs '
  .[0] as $checkpoint |
  ($checkpoint.state.history | map(select(.role == "user")) | last) as $user |
  ($checkpoint.state.history | map(select(.role == "assistant")) | last) as $assistant |
  def marker($prefix; $n): $prefix + (if $n < 10 then "0" else "" end) + ($n|tostring);
  ($checkpoint | .applied_event_sequence = 126 |
   del(.state.history_blocks, .state.history_owned_events, .state.history_hidden_events) |
   .state.history = [range(0;25) as $i |
     ($user | walk(if type == "object" and has("text") then .text = marker("PROMPT"; $i) else . end)),
     ($assistant | walk(if type == "object" and has("text") then .text = marker("ANSWER"; $i) + " PARTA PARTB" else . end))]),
  {record_type:"event", sequence:1, type:"input_journal_v2", data:null},
  (range(0;25) as $i | ($i*5+2) as $start |
    {record_type:"event", sequence:$start, type:"turn_submitted", data:marker("PROMPT"; $i)},
    {record_type:"event", sequence:($start+1), type:"input_consumed", data:($start|tostring)},
    {record_type:"event", sequence:($start+2), type:"assistant_text_delta", data:(marker("ANSWER"; $i)+" PARTA")},
    {record_type:"event", sequence:($start+3), type:"assistant_text_delta", data:" PARTB"},
    {record_type:"event", sequence:($start+4), type:"assistant_text_end", data:null})
' "$seed" >"$root/long.jsonl"
run --import "$root/long.jsonl"; ok
id=$(stdout_text)
before=$(requests)
terminal_start --resume "$id"
expect ANSWER24; expect PARTB
contains "$term/text" 'Showing the latest 20 turns.'
absent "$term/text" PROMPT04; absent "$term/text" ANSWER04
contains "$term/text" PROMPT05; contains "$term/text" ANSWER05
[[ $(grep -c 'ANSWER[0-9][0-9] PARTA PARTB' "$term/text") == 20 ]]
terminal_close
[[ $(requests) == "$before" ]]
jq -se '[.[] | select(.event == "assistant_text_delta" and .replay == true)] | length == 50' "$root/cache/cai/$id.log" >/dev/null
for limit in 0 1 -1 30; do
  terminal_start --resume "$id" --replay-turns "$limit"
  if [[ "$limit" == 0 ]]; then
    absent "$term/text" PROMPT; absent "$term/text" ANSWER
  else
    expect ANSWER24; expect PARTB
    contains "$term/text" PROMPT24
    if [[ "$limit" == 1 ]]; then
      absent "$term/text" PROMPT23; absent "$term/text" ANSWER23
    else
      contains "$term/text" PROMPT00; contains "$term/text" ANSWER00
      absent "$term/text" 'Showing the latest'
    fi
  fi
  terminal_close
done
[[ $(requests) == "$before" ]]
run --export "$id"; ok
json "$root/data/cai/exports/$id/$id.jsonl" 'select(.record_type == "event" and .type == "turn_submitted" and .data == "PROMPT00") | .sequence == 2'
# Runtime restoration is complete even when console replay is suppressed.
plan '{"plan":["CONTEXT RETAINED"]}'
run --resume "$id" --replay-turns 0 -ni 'context check'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" PROMPT00; contains "$root/request" ANSWER00
contains "$root/request" PROMPT24; contains "$root/request" ANSWER24
# INI, environment alias, argv precedence, and numbering reset on /resume.
printf '\n[ui]\nreplay_turns = 1\n' >>"$config"
terminal_start --resume "$id"
expect 'CONTEXT RETAINED'; absent "$term/text" PROMPT24
terminal_close
export CAI_REPLAY_TURNS=0
terminal_start --resume "$id"
absent "$term/text" 'CONTEXT RETAINED'
send "/resume\r"; expect "$id"
send '/resume 1\r'; expect '$ /resume 1'
send '/status\r'; expect '## Status'
absent "$term/text" 'CONTEXT RETAINED'
terminal_close
terminal_start --resume "$id" --replay-turns 1
expect 'CONTEXT RETAINED'
send '/new\r'; expect 'New session'
send '/resume\r'; expect "$id"
send '/resume 2\r'; expect 'CONTEXT RETAINED'
terminal_close
export CAI_REPLAY_TURNS=-2
run --list; bad
