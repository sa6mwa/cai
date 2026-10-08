#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
chat_cli=${1:?chat fixture binary}
printf 'tool-output-marker\n' >"$work/data.txt"
peer_start
for chat in false true; do
  binary="$cli"; [[ "$chat" == false ]] || binary="$chat_cli"
  base[0]="$binary"
  before=$(requests)
  plan "{\"chat\":$chat,\"plan\":[{\"pause\":true,\"tool\":\"read_file\",\"arguments\":{\"path\":\"data.txt\"}},\"BOUNDARY done\"]}"
  terminal_start -N
  send 'begin tool boundary test\r'; expect 'STREAM in flight'
  send '!printf shell-boundary-marker\r'; expect 'Shell command exited 0'
  [[ $(requests) == $((before+1)) ]]
  release; expect_assistant 'BOUNDARY done'; expect 'Worked for'
  [[ $(requests) == $((before+2)) ]]
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  contains "$root/request" shell-boundary-marker
  contains "$root/request" tool-output-marker
  jq -e '[.messages // .input | to_entries[] | select(.value|tostring|contains("<user_shell_command>"))] | length == 1' "$root/request" >/dev/null
  jq -e '(.messages // .input) as $items | [$items|to_entries[]|select(.value.role == "tool" or .value.type == "function_call_output")|.key] as $tools | [$items|to_entries[]|select(.value|tostring|contains("<user_shell_command>"))|.key][0] > $tools[-1]' "$root/request" >/dev/null
  terminal_close
done
