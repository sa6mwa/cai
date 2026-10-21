#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
base+=(--todo)
mkdir -p "$root/config/cai"
printf '[todo]\ndefault_board = configured\n' >"$root/config/cai/config.ini"
export CAI_BOARD=environment
plan '{"plan":[{"tool":"todo_kanban","arguments":{"operation":"current_board"}},"ENV BOARD"]}'
run -Nni 'show the board'; ok
contains "$root/peer/requests.jsonl" 'environment'
# Explicit startup selection overrides the saved/environment board.
plan '{"plan":[{"tool":"todo_kanban","arguments":{"operation":"add_item","title":"PERSISTENT BOARD CARD"}},"ADDED"]}'
run --board startup -ni add; ok
terminal_start
send '/status\r'; expect 'Kanban board'; expect 'startup'
send '/todo\r'; expect 'PERSISTENT BOARD CARD'
send '/todo new project\r'; expect 'Kanban: project'
send '/todo use missing\r'; expect 'board was not found'
send '/todo\r'; expect 'Kanban: project'
send '/todo list\r'; expect 'startup'; expect 'environment'
send '/todo use startup\r'; expect 'PERSISTENT BOARD CARD'
send '/todo new saved\r'; expect 'Kanban: saved'
terminal_close
terminal_start
send '/todo\r'; expect 'Kanban: saved'
# Agent changes and human changes share one selected board and checkpoint.
plan '{"plan":[{"tool":"todo_kanban","arguments":{"operation":"select_board","board_name":"startup"}},"SELECTED STARTUP"]}'
prompt 'select startup' 'SELECTED STARTUP'
send '/todo\r'; expect 'PERSISTENT BOARD CARD'
terminal_close
terminal_start
send '/todo\r'; expect 'PERSISTENT BOARD CARD'
# Busy mutations fail without changing the next tool's board.
plan '{"plan":[{"pause":true,"text":"BUSY DONE"}]}'
send 'busy\r'; expect 'STREAM in flight'
send '/todo new unsafe\r'; expect 'between turns'
release
expect 'BUSY DONE'
terminal_close
terminal_start -N
send '/todo\r'; expect 'Kanban: environment'
terminal_close
unset CAI_BOARD
terminal_start -N
send '/todo\r'; expect 'Kanban: configured'
terminal_close
base+=(--no-todo)
terminal_start --board ignored
send '/todo\r'; expect 'Kanban is disabled'
send '/todo help\r'; expect '/todo new NAME'
terminal_close
run --board; bad; contains "$root/stderr" --board
