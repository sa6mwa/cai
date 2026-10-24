#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
terminal_start -N --messaging --message-queue standalone-recipient
send '/queue\r'; expect 'Message queue: standalone-recipient'
run --list; ok; cp "$root/stdout" "$root/before-list"
text=$'Standalone "quoted" \\ text\nand Unicode 雪'
# No provider flags or credentials: this is a storage utility, not inference.
"$cli" -C "$work" --queue standalone-recipient --message "$text" \
  --message-ack-timeout-ms 2000 >"$root/sent" 2>"$root/send-log"
jq -e '.status == "queued" and .recipient == "standalone-recipient" and (.message_id|length > 0)' "$root/sent" >/dev/null
[[ ! -s "$root/send-log" && $(requests) == 0 ]]
expect 'approval'
send '\033a'; expect 'Alt-Y approve'; contains "$term/text" 'cai.cli.'
logs
jq -se --arg text "$text" 'any(.[]; .event == "agent_message_received" and .msg == $text)' "$root/logs.jsonl" >/dev/null
send '\033n'
send '/queue\r'; expect 'Message queue: standalone-recipient'
run --list; ok; cmp "$root/stdout" "$root/before-list"
for args in missing-text missing-queue mixed empty; do
  case "$args" in
    missing-text) run -q standalone-recipient ;;
    missing-queue) run -M text ;;
    mixed) run -q standalone-recipient -M text -i forbidden ;;
    empty) run -q standalone-recipient -M '' ;;
  esac
  bad; [[ ! -s "$root/stdout" ]]
done
run -q invalid/name -M text; bad; contains "$root/stderr" 'valid queue name'
terminal_close
# Enqueue succeeds without a consumer, ignores the agent's ACK deadline, and
# remains available when a receiver subsequently starts.
run -q offline-recipient -M 'Queued while offline' --message-ack-timeout-ms 1; ok
jq -e '.status == "queued" and .recipient == "offline-recipient"' "$root/stdout" >/dev/null
terminal_start -N --messaging --message-queue offline-recipient
expect 'approval'
send '\033a'; expect 'Alt-Y approve'; contains "$term/text" 'Queued while offline'
[[ $(requests) == 0 ]]
terminal_close
