#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_MESSAGES_START_FAILURE=1
terminal_start -N --message-queue toggle-recovery
send '/queue on\r'; expect 'Injected message listener startup failure'
send '/queue\r'; expect 'Messaging is disabled'
expect 'incoming queue.'
# The suggestion is inline code rendered by Markdown, not literal backticks
# or a plain notice. Match SGR at the code span without pinning theme colors.
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/hint"
absent "$root/hint" '`/queue on`'
grep -Eq $'\033\\[[0-9;]*m/queue on\033\\[0m' "$term/raw"
send '/status\r'; expect 'Message queue'; expect 'disabled'
send '/queue on\r'; expect 'Message queue: toggle-recovery'
send '/tools\r'; expect 'agent_message'
send '/queue off\r'; expect 'Messaging is disabled'
send '/tools\r'; expect 'No MCP servers configured'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/tools"
absent "$root/tools" agent_message
[[ $(requests) == 0 ]]
run --list; ok
[[ $(wc -l <"$root/stdout") == 1 ]]
terminal_close
