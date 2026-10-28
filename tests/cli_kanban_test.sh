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
# Agent changes and human changes share one selected board and commit.
plan '{"plan":[{"tool":"todo_kanban","arguments":{"operation":"select_board","board_name":"startup"}},"SELECTED STARTUP"]}'
prompt 'select startup' 'SELECTED STARTUP'
contains "$term/feed" '* Selected board startup'
absent "$term/feed" '* Completed todo_kanban'
absent "$term/feed" '"operation":"select_board"'
send '/todo\r'; expect 'PERSISTENT BOARD CARD'
terminal_close
terminal_start
send '/todo\r'; expect 'PERSISTENT BOARD CARD'
# Busy board changes remain queued until a complete turn boundary.
plan '{"plan":[{"pause":true,"text":"BUSY DONE"}]}'
send 'busy\r'; expect 'STREAM in flight'
send '/todo new deferred\033\r'; expect 'Q 1. /todo new deferred'
release
expect 'BUSY DONE'
expect 'Kanban: deferred'
absent "$term/feed" 'between turns'
terminal_close
terminal_start -N
send '/todo\r'; expect 'Kanban: environment'
terminal_close
unset CAI_BOARD
terminal_start -N
send '/todo\r'; expect 'Kanban: configured'
terminal_close
plan '{"plan":[{"tool":"todo_kanban","arguments":{"operation":"add_item","title":"Fix Snake speed progression until FINAL_TITLE_SUFFIX","description":"**TASK_CONTEXT**\n\n- Preserve `game.c`.\n\n| Check | Expected |\n| --- | --- |\n| Speed | Stable |"}},{"tool":"todo_kanban","arguments":{"operation":"add_item","title":"Moving card"}},{"tool":"todo_kanban","arguments":{"operation":"add_item","title":"Finished card","description":"Finished context"}},{"tool":"todo_kanban","arguments":{"operation":"move_item","item_id":"DEF-002","status":"in_process"}},{"tool":"todo_kanban","arguments":{"operation":"complete_item","item_id":"DEF-003"}},"LAYOUT SEEDED"]}'
run --board default -Nni 'seed card layouts'; ok
terminal_start --board default
before=$(requests)
send '/todo\r'; expect 'TODO'; expect 'DOING'; expect 'DONE'
expect 'DEF-001'; expect 'DEF-002'; expect 'DEF-003'; expect 'Fix Snake'
absent "$term/feed" FINAL_TITLE_SUFFIX
absent "$term/feed" TASK_CONTEXT
send '/todo layout list\r'; expect 'Title'; expect FINAL_TITLE_SUFFIX
absent "$term/feed" TASK_CONTEXT
send '/todo see def-001\r'; expect 'DEF-001 (TODO)'; expect FINAL_TITLE_SUFFIX
expect TASK_CONTEXT; expect 'Preserve game.c.'; expect 'Stable'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/details"
absent "$root/details" '**TASK_CONTEXT**'
send '/todo see DEF-002\r'; expect 'DEF-002 (DOING)'; expect 'Moving card'
send '/todo see DEF-003\r'; expect 'DEF-003 (DONE)'; expect 'Finished context'
send '/todo see\r'; expect 'Usage: /todo see ID'
send '/todo see DEF-001 extra\r'; expect 'Usage: /todo see ID'
send '/todo see DEF-999\r'; expect 'item was not found'
send '/todo new other\r'; expect 'Kanban: other'
send '/todo see DEF-001\r'; expect 'item was not found'
send '/todo use default\r'; expect 'Title'; expect FINAL_TITLE_SUFFIX
send '/todo layout invalid\r'; expect 'Usage: /todo layout'
send '/todo layout\r'; expect 'Kanban layout:'; expect 'list'
send '/new\r'; expect 'New session'
send '/todo\r'; expect 'Title'; expect FINAL_TITLE_SUFFIX
send '/todo layout board\r'; expect 'DOING'
[[ $(requests) == "$before" ]]
terminal_close
# Startup config, environment and argv choose the initial presentation; slash
# commands do not persist a UI preference in the conversation or INI file.
printf '[todo]\ndefault_board = configured\n[ui]\ntodo_layout = list\n' >"$root/config/cai/config.ini"
terminal_start --board default
send '/todo\r'; expect 'Title'; expect FINAL_TITLE_SUFFIX
terminal_close
export CAI_TODO_LAYOUT=board
terminal_start --board default
send '/todo\r'; expect 'DOING'
terminal_close
terminal_start --board default --todo-layout list
send '/todo\r'; expect 'Title'; expect FINAL_TITLE_SUFFIX
terminal_close
unset CAI_TODO_LAYOUT
run --todo-layout invalid; bad; contains "$root/stderr" --todo-layout
export CAI_TODO_LAYOUT=invalid
run --list; bad; contains "$root/stderr" CAI_TODO_LAYOUT
unset CAI_TODO_LAYOUT
base+=(--no-todo)
terminal_start --board ignored
send '/todo\r'; expect 'Kanban is disabled'
send '/todo see DEF-001\r'; expect 'Kanban is disabled'
send '/todo help\r'; expect '/todo new NAME'
terminal_close
run --board; bad; contains "$root/stderr" --board
