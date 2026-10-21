#!/usr/bin/env bash
# Two real embedders, C/static and Lua/shared, with a shared encrypted Pouch.
# The HTTP peer supplies model responses; both applications execute real tools.
source "$(dirname "$0")/cli_test_common.sh"
lua_host=$(realpath "$1")
app_pids=() app_terms=() app_events=()
finish() {
  local status=$? pid
  for pid in "${app_pids[@]}"; do
    if [[ "$terminal_pid" == "$pid" ]]; then terminal_pid=; fi
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  done
  # cleanup also owns a current terminal whose startup failed before it could
  # be recorded in app_pids.
  cleanup "$status"
}
trap finish EXIT
peer_start
mkdir -p "$root/lua-work"
common=(-p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-terminal --no-image-generation
        --messaging --message-ack-timeout-ms 2000 --lockd "pouch://$root/state/cai/pouch")
base=("$cli" "${common[@]}" -C "$work")
plan '{"plan":["C READY"]}'
run -Nni 'C conversation'; ok
run --resume; ok
c_id=$(awk '$1 == "1" {print $2; exit}' "$root/stdout")
[[ -n "$c_id" ]]
terminal_start --resume "$c_id"
app_pids+=("$terminal_pid") app_terms+=("$term") app_events+=("$terminal_event_fd")
base=("$lua_host" "${common[@]}" -C "$root/lua-work")
plan '{"plan":["LUA READY"]}'
run -Nni 'Lua conversation'; ok
run --resume; ok
lua_id=$(awk '$1 == "1" {print $2; exit}' "$root/stdout")
[[ -n "$lua_id" && "$lua_id" != "$c_id" ]]
terminal_start --resume "$lua_id"
app_pids+=("$terminal_pid") app_terms+=("$term") app_events+=("$terminal_event_fd")
select_app() {
  term=${app_terms[$1]}
  terminal_pid=${app_pids[$1]}
  terminal_event_fd=${app_events[$1]}
  terminal_events=1
  send_offset=0 send_feed_offset=0
  exec 7<>"$term/input"
}
# Discovery lists both conversations, independently of their directories.
select_app 0
send '/status\r'; expect 'Message queue'
c_queue=$(grep -oE 'cai\.work\.[0-9a-f]{6}' "$term/feed" | head -1)
select_app 1
send '/status\r'; expect 'Message queue'
lua_queue=$(grep -oE 'cai\.lua-work\.[0-9a-f]{6}' "$term/feed" | head -1)
[[ -n "$c_queue" && -n "$lua_queue" && "$c_queue" != "$lua_queue" ]]
plan '{"plan":[{"tool":"agent_message","arguments":{"operation":"list"}},"DESTINATIONS LISTED"]}'
select_app 0
prompt 'list message destinations' 'DESTINATIONS LISTED'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" "$c_id"
contains "$root/request" "$lua_id"
contains "$root/request" "$c_queue"
contains "$root/request" "$lua_queue"
# C -> Lua: the C tool succeeds while Lua is idle and approval is still held.
plan "$(jq -n --arg id "$lua_queue" '{plan:[{tool:"agent_message",arguments:{operation:"send",recipient:$id,text:"PING FROM C"}},"C PING DELIVERED"]}')"
before=$(requests)
prompt 'send a ping to Lua' 'C PING DELIVERED'
[[ $(requests) == $((before+2)) ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
jq -e --arg id "$lua_queue" '[.input[]? | select(.type=="function_call_output") | .output] | last | fromjson | .status=="delivered" and .recipient==$id' "$root/request" >/dev/null
select_app 1
expect 'approval'
contains "$term/text" 'PING FROM C'
# Approve Lua's ordinary next turn, which sends a reply to the still-running C
# conversation. C must ACK it without needing its own approval/model turn.
plan "$(jq -n --arg id "$c_queue" '{plan:[{tool:"agent_message",arguments:{operation:"send",recipient:$id,text:"PONG FROM LUA"}},"LUA REPLY DELIVERED"]}')"
offset=$(wc -c <"$term/text")
send '\033a\033y'; expect_turn "$offset" 'LUA REPLY DELIVERED'
[[ $(requests) == $((before+4)) ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'PING FROM C'
jq -e --arg id "$c_queue" '[.input[]? | select(.type=="function_call_output") | .output] | last | fromjson | .status=="delivered" and .recipient==$id' "$root/request" >/dev/null
select_app 0
expect 'approval'
contains "$term/text" 'PONG FROM LUA'
plan '{"plan":["C RECEIVED LUA REPLY"]}'
offset=$(wc -c <"$term/text")
send '\033a\033y'; expect_turn "$offset" 'C RECEIVED LUA REPLY'
[[ $(requests) == $((before+5)) ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'PONG FROM LUA'
# The same original app processes stayed alive for the whole round trip.
[[ ! -s "${app_terms[0]}/status" && ! -s "${app_terms[1]}/status" ]]
logs
jq -se 'any(.[]; .event=="agent_message_received" and .role=="agent" and .msg=="PING FROM C") and any(.[]; .event=="agent_message_received" and .role=="agent" and .msg=="PONG FROM LUA")' "$root/logs.jsonl" >/dev/null
# Keep an unapproved delivery in the original conversation, then start a new
# conversation for the same directory. Its public queue name must stay stable.
select_app 1
plan "$(jq -n --arg queue "$c_queue" '{plan:[{tool:"agent_message",arguments:{operation:"send",recipient:$queue,text:"HELD BEFORE TAKEOVER"}},"BEFORE TAKEOVER DELIVERED"]}')"
prompt 'send a message before takeover' 'BEFORE TAKEOVER DELIVERED'
select_app 0
expect 'HELD BEFORE TAKEOVER'
base=("$lua_host" "${common[@]}" -C "$work")
terminal_start -N
app_pids+=("$terminal_pid") app_terms+=("$term") app_events+=("$terminal_event_fd")
send '/status\r'; expect 'Message queue'
contains "$term/feed" "$c_queue"
absent "$term/text" 'HELD BEFORE TAKEOVER'
select_app 1
plan "$(jq -n --arg queue "$c_queue" '{plan:[{tool:"agent_message",arguments:{operation:"send",recipient:$queue,text:"AFTER TAKEOVER"}},"AFTER TAKEOVER DELIVERED"]}')"
prompt 'send to the new mailbox owner' 'AFTER TAKEOVER DELIVERED'
select_app 2
expect 'AFTER TAKEOVER'
plan '{"plan":["NEW OWNER PROCESSED"]}'
offset=$(wc -c <"$term/text")
send '\033a\033y'; expect_turn "$offset" 'NEW OWNER PROCESSED'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'AFTER TAKEOVER'
absent "$root/request" 'HELD BEFORE TAKEOVER'
select_app 0
absent "$term/feed" 'AFTER TAKEOVER'
plan '{"plan":["OLD DELIVERED MESSAGE PROCESSED"]}'
offset=$(wc -c <"$term/text")
send '\033a\033y'; expect_turn "$offset" 'OLD DELIVERED MESSAGE PROCESSED'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'HELD BEFORE TAKEOVER'
absent "$root/request" 'AFTER TAKEOVER'
# Exiting the displaced process must not clear the new owner's binding.
terminal_close; app_pids[0]=
select_app 1
plan "$(jq -n --arg queue "$c_queue" '{plan:[{tool:"agent_message",arguments:{operation:"send",recipient:$queue,text:"AFTER OLD OWNER EXIT"}},"NEW BINDING SURVIVED"]}')"
prompt 'send after the old owner exits' 'NEW BINDING SURVIVED'
select_app 2
expect 'AFTER OLD OWNER EXIT'
terminal_close; app_pids[2]=
select_app 1
terminal_close; app_pids[1]=
