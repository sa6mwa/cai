#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["ID FIRST"]}')"
terminal_start -N
send '/status\r'; expect 'Session ID'; expect 'Last response ID'; expect 'Not available'; expect 'Use /tools'
prompt 'first response' 'ID FIRST'
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
run --export "$id"; ok
exported="$root/data/cai/exports/$id/$id.jsonl"
response=$(jq -r 'select(.record_type == "commit") | .state.metadata.previous_response_id' "$exported")
[[ -n "$response" && "$response" != null ]]
before=$(requests)
send '/status\r'; expect "$id"; expect "$response"; expect 'Use /tools'
[[ $(requests) == "$before" ]]
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{pause:true,pause_text:"WORK IN FLIGHT",text:"ID SECOND"}]}')"
send 'second response\r'; expect 'WORK IN'
send '/status\r'; expect "$id"; expect "$response"; expect 'Use /tools'
release; expect_turn "$send_offset" 'ID SECOND'
terminal_close
run --export "$id"; ok
latest=$(jq -r 'select(.record_type == "commit") | .state.metadata.previous_response_id' "$exported")
[[ -n "$latest" && "$latest" != null ]]
if [[ "$chat" == false ]]; then [[ "$latest" != "$response" ]]; fi
before=$(requests)
terminal_start --resume "$id"
send '/status\r'; expect "$id"; expect "$latest"; expect 'Use /tools'
send '/new\r'; expect 'New session'
send '/status\r'; expect 'Last response ID'; expect 'Not available'; expect 'Use /tools'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/new-status"
absent "$root/new-status" "$id"
absent "$root/new-status" "$latest"
[[ $(requests) == "$before" ]]
terminal_close
