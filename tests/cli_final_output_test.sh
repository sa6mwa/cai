#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
protocol=${1:-responses}
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
for final in '""' '{"reasoning":["Reasoning without a final reply"]}' '{"error":"Final turn failed"}'; do
  plan "{\"chat\":$chat,\"plan\":[\"STALE FIRST REPLY\",$final]}"
  run -Nni first -i second
  if [[ "$final" == *'Final turn failed'* ]]; then bad; else ok; fi
  [[ ! -s "$root/stdout" ]]
  contains "$root/stderr" 'STALE FIRST REPLY'
done
plan "{\"chat\":$chat,\"plan\":[\"STALE FIRST REPLY\",{\"text\":\"Current partial reply\",\"error\":\"Failure after partial reply\"}]}"
run -Nni first -i second; bad
[[ $(stdout_text) == 'Current partial reply' ]]
absent "$root/stdout" 'STALE FIRST REPLY'

# A tool continuation is a new response within the same turn, even when it
# emits no assistant text. Its preamble must remain in logs, not stdout.
printf 'fixture contents\n' >"$work/input.txt"
for final in '""' '{"reasoning":["Reasoning-only tool continuation"]}'; do
  plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"read_file\",\"arguments\":{\"path\":\"input.txt\"},\"text\":\"STALE TOOL PREAMBLE\"},$final]}"
  run -Nni 'read input.txt'; ok
  [[ ! -s "$root/stdout" ]]
  contains "$root/stderr" 'STALE TOOL PREAMBLE'
done
