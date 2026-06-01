#!/usr/bin/env bash
set -euo pipefail

repo_root=$1
basic_example=$2
streaming_example=$3
history_example=$4
session_example=$5
terminal_chat_example=$6
lua_executable=$7

tmpdir=$(mktemp -d)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT

cd "$repo_root"

strip_ansi() {
  env -u LD_PRELOAD sed -E 's/\x1B\[[0-9;]*[[:alpha:]]//g' "$1" >"$2"
}

"$basic_example" >"$tmpdir/basic.out"
grep -F 'hello from cai' "$tmpdir/basic.out" >/dev/null

"$streaming_example" >"$tmpdir/streaming.out"
test -s "$tmpdir/streaming.out"

"$history_example" >"$tmpdir/history.out"
grep -F '[' "$tmpdir/history.out" >/dev/null
grep -F '"role":"user"' "$tmpdir/history.out" >/dev/null

"$session_example" "$tmpdir/session.json" >"$tmpdir/session.out"
grep -F 'saved:' "$tmpdir/session.out" >/dev/null
grep -F 'restored:' "$tmpdir/session.out" >/dev/null
grep -F '"version"' "$tmpdir/session.json" >/dev/null

printf 'hello\n/exit\n' | env \
  CAI_TODO_STORE="$tmpdir/terminal-todo.json" \
  CAI_TODO_LOCK="$tmpdir/terminal-todo.lock" \
  "$terminal_chat_example" \
  >"$tmpdir/terminal.out" 2>&1
strip_ansi "$tmpdir/terminal.out" "$tmpdir/terminal.clean"
grep -F 'hint: pass --exec-tool-dir <path> to enable exec_command rooted to that path' \
  "$tmpdir/terminal.clean" >/dev/null
grep -F 'hint: pass --read-tool-dir <path> to enable' \
  "$tmpdir/terminal.clean" >/dev/null
grep -F '> ' "$tmpdir/terminal.clean" >/dev/null
grep -F '[response]' "$tmpdir/terminal.clean" >/dev/null
grep -F '[usage]' "$tmpdir/terminal.clean" >/dev/null

make lua-rock >/dev/null
eval "$(make -s lua-env)"
printf 'hello\n/exit\n' | env \
  CAI_LUA_TODO_STORE="$tmpdir/lua-terminal-todo.json" \
  CAI_LUA_TODO_LOCK="$tmpdir/lua-terminal-todo.lock" \
  "$lua_executable" examples/lua-terminal-chat/main.lua \
  >"$tmpdir/lua-terminal.out" 2>&1
strip_ansi "$tmpdir/lua-terminal.out" "$tmpdir/lua-terminal.clean"
grep -F 'hint: pass --exec-tool-dir <path> to enable exec_command rooted to that path' \
  "$tmpdir/lua-terminal.clean" >/dev/null
grep -F 'hint: pass --read-tool-dir <path> to enable' \
  "$tmpdir/lua-terminal.clean" >/dev/null
grep -F '> ' "$tmpdir/lua-terminal.clean" >/dev/null
grep -F '[response]' "$tmpdir/lua-terminal.clean" >/dev/null
grep -F '[usage]' "$tmpdir/lua-terminal.clean" >/dev/null
