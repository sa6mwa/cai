#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
chat_cli=${1:?chat fixture binary}
peer_start
printf 'KEEP_TOOL_RESULT\n' >"$work/keep.txt"
printf 'DROP_TOOL_RESULT\n' >"$work/drop.txt"
unicode_reply='KEEPRESPONSE '
for ((i=0; i<700; i++)); do unicode_reply+='€'; done
for chat in false true; do
  binary="$cli"; [[ "$chat" == false ]] || binary="$chat_cli"
  base[0]="$binary"
  plan "$(jq -nc --argjson chat "$chat" --arg reply "$unicode_reply" \
    '{chat:$chat,plan:[{tool:"read_file",arguments:{path:"keep.txt"}},$reply]}')"
  terminal_start -N
  prompt 'KEEP_PROMPT' 'KEEPRESPONSE'
  send '!printf KEEP_SHELL\r'; expect 'Shell command exited 0'
  plan "{\"chat\":$chat,\"plan\":[{\"pause\":true,\"tool\":\"read_file\",\"arguments\":{\"path\":\"drop.txt\"}},\"DROPRESPONSE\"]}"
  send 'DROP_PROMPT\r'; expect 'STREAM in flight'
  send '/trim 1\r'; expect 'require an idle runtime'
  send 'DROP_STEERING\r'
  release; expect_assistant 'DROPRESPONSE'; expect 'Worked for'
  send '/trim\r'; expect '1. DROPRESPONSE'; expect '2. Shell: printf KEEP_SHELL'; expect '3. KEEPRESPONSE'
  send '/trim 0\r'; expect 'Trimmed 0 newest history entries'
  send '/trim -1\r'; expect 'Usage: /trim'
  send '/trim 999999999999999999999999\r'; expect 'nonnegative integer'
  send '/trim 4\r'; expect 'exceeds retained history entries'
  send '/trim 1\r'; expect 'Trimmed 1 newest history entry'
  send '/trim\r'; expect '1. Shell: printf KEEP_SHELL'; expect '2. KEEPRESPONSE'
  send '/export\r'; expect '.jsonl'
  terminal_close
  journal=$(find "$root/data/cai/exports" -name '*.jsonl' -type f | sort | tail -1)
  contains "$journal" KEEP_PROMPT
  contains "$journal" KEEP_TOOL_RESULT
  contains "$journal" KEEP_SHELL
  absent "$journal" DROP_PROMPT
  absent "$journal" DROPRESPONSE
  absent "$journal" DROP_STEERING
  absent "$journal" DROP_TOOL_RESULT
  markdown=${journal%.jsonl}.md
  contains "$markdown" KEEPRESPONSE
  absent "$markdown" DROPRESPONSE
  original_id=$(basename "$journal" .jsonl)
  # Exercise a saved active conversation without the new entry index. The
  # actual journal still identifies turns, steering and shell executions.
  jq -c 'if .record_type == "checkpoint" then .state |= del(.history_blocks, .history_owned_events) else . end' "$journal" >"$root/unindexed.jsonl"
  run --import "$root/unindexed.jsonl"; ok
  plan "{\"chat\":$chat,\"plan\":[\"AFTERTRIM\"]}"
  terminal_start
  absent "$term/text" DROPRESPONSE
  contains "$term/text" KEEPRESPONSE
  prompt 'NEXT_PROMPT' 'AFTERTRIM'
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  contains "$root/request" KEEP_PROMPT
  contains "$root/request" KEEP_TOOL_RESULT
  contains "$root/request" KEEP_SHELL
  absent "$root/request" DROP_PROMPT
  absent "$root/request" DROP_STEERING
  absent "$root/request" DROP_TOOL_RESULT
  send '/trim 2\r'; expect 'Trimmed 2 newest history entries'
  send '/trim\r'; expect '1. KEEPRESPONSE'
  terminal_close
  # The cut belongs solely to the active imported conversation.
  run --export "$original_id"; ok
  contains "$journal" KEEP_SHELL
done
