#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
base+=(--message-queue receiver-workspace -r low)
sender_pid=
finish() {
  local status=$?
  if [[ -n "$sender_pid" ]]; then kill "$sender_pid" 2>/dev/null || true; wait "$sender_pid" 2>/dev/null || true; fi
  cleanup "$status"
}
trap finish EXIT
mkdir -p "$root/sender-peer" "$root/sender-work"
printf '%s\n' '{"plan":[]}' >"$root/sender-peer/config.json"
"${CAI_TEST_PYTHON:-python3}" "$support/fixtures/http_peer.py" "$root/sender-peer" >"$root/sender-peer/stdout" 2>"$root/sender-peer/stderr" & sender_pid=$!
wait_for test -s "$root/sender-peer/port"
sender_endpoint="http://127.0.0.1:$(cat "$root/sender-peer/port")/v1"
sender_base=("$cli" -p custom --endpoint "$sender_endpoint" -m gpt-5.6-luna --no-terminal --no-image-generation --messaging --message-ack-timeout-ms 2000 --message-queue sender-workspace -C "$root/sender-work")
plan '{"plan":["RECEIVER READY"]}'
run -Nni 'receiver setup'; ok
jq -e 'all(.tools[]?; .name != "agent_message")' "$root/peer/requests.jsonl" >/dev/null
run --list; ok
receiver=$(awk 'NF {print $1; exit}' "$root/stdout")
[[ -n "$receiver" ]]
send_message() {
  local text=$1 recipient=${2:-receiver-workspace}
  jq -n --arg recipient "$recipient" --arg text "$text" '{plan:[{tool:"agent_message",arguments:{operation:"send",recipient:$recipient,text:$text}},"SENDER DONE"]}' >"$root/sender-peer/config.tmp"
  mv "$root/sender-peer/config.tmp" "$root/sender-peer/config.json"
  "${sender_base[@]}" -Nni 'send the agent message' >"$root/sender.stdout" 2>"$root/sender.stderr" || { cat "$root/sender.stderr" >&2; fail 'Sender failed'; }
  contains "$root/sender.stdout" 'SENDER DONE'
  jq -se 'any(.[] | .. | strings; contains("\"status\":\"delivered\""))' "$root/sender-peer/requests.jsonl" >/dev/null
}
terminal_start --resume "$receiver"
send '/queue\r'; expect 'Messaging is disabled'
# Enabling in the running TUI must retain the session and not repeat history
# or make an inference request. The real tool catalog changes too.
count=$(requests)
send '/queue on\r'; expect 'Message queue: receiver-workspace'
send '/queue on\r'; expect 'Message queue: receiver-workspace'
[[ $(requests) == "$count" ]]
[[ $(grep -c 'RECEIVER READY' "$term/text") == 1 ]]
absent "$term/feed" 'RECEIVER READY'
send '/tools\r'; expect 'agent_message'
send '/status\r'; expect 'receiver-workspace'; expect 'off (manual approval)'
send '/model gpt-6-luna high\r'; expect 'reasoning effort high'
send '/queue off\r'; expect 'Messaging is disabled'
send '/queue off\r'; expect 'Messaging is disabled'
send '/tools\r'; expect 'No MCP servers configured'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/disabled-tools"
absent "$root/disabled-tools" agent_message
send '/status\r'; expect 'Message queue'; expect 'disabled'
send '/queue on\r'; expect 'Message queue: receiver-workspace'
send '/model\r'; expect 'Current: gpt-6-luna'; expect 'Reasoning effort: high'
[[ $(requests) == "$count" ]]
send '/queue\r'; expect 'Message queue: receiver-workspace'
count=$(requests)
send_message 'External hello one'
expect 'approval'
# Arrival is delivered/ACKed without producing a model request.
[[ $(requests) == "$count" ]]
send '/queue off\r'; expect 'Approve or reject queued incoming messages'
send '\033a'; expect 'Alt-Y approve'
contains "$term/text" 'External hello one'
send '\033e'; expect_prompt 'External hello one'
send ' edited\r'; expect 'approval'
[[ $(requests) == "$count" ]]
plan '{"plan":["RECEIVER ONE DONE"]}'
offset=$(wc -c <"$term/text")
send '\033a\033y'; expect_turn "$offset" 'RECEIVER ONE DONE'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'External hello one edited'
contains "$root/request" '[Agent message '
contains "$root/request" 'receiver setup'
json "$root/request" '.model == "gpt-6-luna" and .reasoning.effort == "high" and any(.tools[]?; .name == "agent_message")'
run --list; ok
[[ $(awk 'NF {print $1; exit}' "$root/stdout") == "$receiver" ]]
# An approved message waits for the current turn, then precedes automatic
# goal continuation. It must not wait for the entire goal to finish.
plan '{"plan":[{"pause":true,"text":"BUSY TURN DONE"}]}'
send '/goal new coordinate with the sender\r'; expect 'STREAM in flight'
send '/queue not-between-turns\033\r'; expect 'S 1. /queue not-between-turns'
send '/queue off\033\r'; expect 'S 2. /queue off'
send '/queue\r'; expect 'Message queue: receiver-workspace'
send_message 'External hello two'
expect 'approval'
send '\033a\033y'
count=$(requests)
send '/status\r'; expect 'Metric'
[[ $(requests) == "$count" ]]
plan_config='{"plan":[{"tool":"update_goal","arguments":{"status":"complete"}},"QUEUED INBOUND DONE"]}'
printf '%s\n' "$plan_config" >"$root/peer/config.tmp"
mv "$root/peer/config.tmp" "$root/peer/config.json"
offset=$(wc -c <"$term/text")
release
expect_turn "$offset" 'QUEUED INBOUND DONE'
contains "$term/feed" 'Change the message queue between turns'
contains "$term/feed" 'Change messaging between turns'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'External hello two'
sed -n "$((count+1))p" "$root/peer/requests.jsonl" >"$root/next-request"
jq -e '[.input[]? | select(.role=="user") | .content[]? | .text?] | last | contains("External hello two")' "$root/next-request" >/dev/null
# Enabling autoqueue approves waiting messages and applies to future arrivals.
send '/autoqueue on\r'; expect 'Autoqueue on'
send '/status\r'; expect 'on (automatic approval)'
send '/queue off\r'; expect 'Messaging is disabled'
send '/queue on\r'; expect 'Message queue: receiver-workspace'
send '/status\r'; expect 'on (automatic approval)'
plan '{"plan":["AUTOMATIC INBOUND DONE"]}'
offset=$(wc -c <"$term/text")
send_message 'Automatically accepted incoming prompt'
expect_turn "$offset" 'AUTOMATIC INBOUND DONE'
send '/autoqueue off\r'; expect 'Autoqueue off'
send '/queue New-Receiver\r'; expect 'Message queue: new-receiver'
send '/queue bad/name\r'; expect 'Usage: /queue NAME'
send '/queue two names\r'; expect 'Usage: /queue NAME'
send '/queue\r'; expect 'Message queue: new-receiver'
send '/status\r'; expect 'new-receiver'
count=$(requests)
send_message 'Rejected incoming prompt' new-receiver
expect 'approval'
send '\033a\033n'
send '/status\r'; expect 'Metric'
[[ $(requests) == "$count" ]]
send '/new\r'; expect 'Message queue:'; expect 'new-receiver'; expect 'ID:'
absent "$term/feed" '**ID:**'
send '/queue\r'; expect 'Message queue: new-receiver'
send "/resume 1\r"
send '/queue\r'; expect 'Message queue: new-receiver'
terminal_close
base+=(--messaging)
terminal_start --resume "$receiver"
send '/status\r'; expect 'Metric'
[[ $(requests) == "$count" ]]
terminal_close
logs
jq -se 'any(.[]; .event=="agent_message_received" and .role=="agent" and .msg=="External hello one")' "$root/logs.jsonl" >/dev/null
[[ ! -s "$root/sender-peer/stderr" ]]
