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
attempts=${CAI_EXAMPLES_LIVE_ATTEMPTS:-3}
command_timeout_sec=${CAI_EXAMPLES_LIVE_COMMAND_TIMEOUT_SEC:-45}
suite_deadline_sec=${CAI_EXAMPLES_LIVE_SUITE_DEADLINE_SEC:-210}
suite_started_at=$(date +%s)
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT
dump_failure() {
  local status=$?

  if ((status == 0)); then
    return
  fi
  for file in "$tmpdir"/*.clean "$tmpdir"/*.out; do
    if [[ -f "$file" ]]; then
      printf '\n--- %s ---\n' "$file" >&2
      sed -n '1,240p' "$file" >&2 || true
    fi
  done
  return "$status"
}
trap dump_failure ERR

cd "$repo_root"

strip_ansi() {
  env -u LD_PRELOAD sed -E 's/\x1B\[[0-9;]*[[:alpha:]]//g' "$1" >"$2"
}

assert_count_at_least() {
  local pattern=$1
  local minimum=$2
  local file=$3
  local count

  count=$(grep -F -c "$pattern" "$file" || true)
  if ((count < minimum)); then
    printf 'expected at least %d occurrence(s) of %s in %s, saw %d\n' \
      "$minimum" "$pattern" "$file" "$count" >&2
    return 1
  fi
}

is_transient_live_provider_failure() {
  local file=$1

  grep -E \
    'upstream connect error|disconnect/reset before headers|connection timeout|reset reason: connection timeout|HTTP request transport failed|Timeout was reached|timed out|timeout' \
    "$file" >/dev/null 2>&1
}

run_with_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --foreground --kill-after=5s "${command_timeout_sec}s" "$@"
  else
    "$@"
  fi
}

run_live_capture() {
  local label=$1
  local output=$2
  local attempt
  local elapsed
  local status

  shift 2
  for ((attempt = 1; attempt <= attempts; attempt++)); do
    elapsed=$(($(date +%s) - suite_started_at))
    if ((elapsed >= suite_deadline_sec)); then
      printf '[examples-live-smoke] refusing to start %s: suite budget exhausted after %ds\n' \
        "$label" "$elapsed" >&2
      return 124
    fi
    if run_with_timeout "$@" >"$output" 2>&1; then
      return 0
    fi
    status=$?
    if ((status == 124)); then
      printf '[examples-live-smoke] %s attempt=%d exceeded %ss command timeout\n' \
        "$label" "$attempt" "$command_timeout_sec" >&2
    fi
    if { ((status != 124)) && ! is_transient_live_provider_failure "$output"; } ||
      ((attempt == attempts)); then
      return "$status"
    fi
    printf '[examples-live-smoke] retrying %s after transient provider failure attempt=%d\n' \
      "$label" "$((attempt + 1))" >&2
    sleep "$((attempt * 2))"
  done
  return 1
}

write_terminal_fixture() {
  local root=$1

  mkdir -p "$root/subdir"
  printf 'NOTE_ALPHA from read_file fixture\n' >"$root/note.txt"
  printf 'nested fixture\n' >"$root/subdir/nested.txt"
}

assert_terminal_multi_turn_transcript() {
  local transcript=$1

  assert_count_at_least '> ' 5 "$transcript"
  assert_count_at_least '[response]' 5 "$transcript"
  assert_count_at_least '[usage]' 5 "$transcript"
  assert_count_at_least '[tool]' 4 "$transcript"
  grep -F '[tool] todo_kanban input=' "$transcript" >/dev/null
  grep -F '[tool] todo_kanban output=' "$transcript" >/dev/null
  grep -F '[tool] list_files input=' "$transcript" >/dev/null
  if grep -F '[tool] list_files input={}' "$transcript" >/dev/null; then
    printf 'list_files printed empty tool input in %s\n' "$transcript" >&2
    return 1
  fi
  grep -F '[tool] list_files output=' "$transcript" >/dev/null
  grep -F '[tool] read_file input=' "$transcript" >/dev/null
  if grep -F '[tool] read_file input={}' "$transcript" >/dev/null; then
    printf 'read_file printed empty tool input in %s\n' "$transcript" >&2
    return 1
  fi
  grep -F '[tool] read_file output=' "$transcript" >/dev/null
  grep -F '[tool] exec_command input=' "$transcript" >/dev/null
  if grep -F '[tool] exec_command input={}' "$transcript" >/dev/null; then
    printf 'exec_command printed empty tool input in %s\n' "$transcript" >&2
    return 1
  fi
  grep -F '"stdin"' "$transcript" >/dev/null
  grep -F '[tool] exec_command output=' "$transcript" >/dev/null
  grep -F 'CAI_MULTI_TURN_ALPHA' "$transcript" >/dev/null
  grep -F 'NOTE_ALPHA' "$transcript" >/dev/null
  grep -F 'EXEC_ALPHA' "$transcript" >/dev/null
}

tool_gate_prefix='Release-gate tool-use test. You MUST call exactly the named tool before answering. If you answer without that tool call, the test fails.'

run_live_capture basic "$tmpdir/basic.out" "$basic_example"
grep -F 'hello from cai' "$tmpdir/basic.out" >/dev/null

run_live_capture streaming "$tmpdir/streaming.out" "$streaming_example"
test -s "$tmpdir/streaming.out"

run_live_capture history "$tmpdir/history.out" "$history_example"
grep -F '[' "$tmpdir/history.out" >/dev/null
grep -F '"role":"user"' "$tmpdir/history.out" >/dev/null

run_live_capture session "$tmpdir/session.out" "$session_example" "$tmpdir/session.json"
grep -F 'saved:' "$tmpdir/session.out" >/dev/null
grep -F 'restored:' "$tmpdir/session.out" >/dev/null
grep -F '"version"' "$tmpdir/session.json" >/dev/null

printf 'hello\n/exit\n' >"$tmpdir/terminal.input"
run_live_capture terminal "$tmpdir/terminal.out" env \
  CAI_TODO_STORE="$tmpdir/terminal-todo.json" \
  CAI_TODO_LOCK="$tmpdir/terminal-todo.lock" \
  "$terminal_chat_example" \
  <"$tmpdir/terminal.input"
strip_ansi "$tmpdir/terminal.out" "$tmpdir/terminal.clean"
grep -F 'hint: pass --exec-tool-dir <path> to enable exec_command rooted to that path' \
  "$tmpdir/terminal.clean" >/dev/null
grep -F 'hint: pass --read-tool-dir <path> to enable' \
  "$tmpdir/terminal.clean" >/dev/null
grep -F '> ' "$tmpdir/terminal.clean" >/dev/null
grep -F '[response]' "$tmpdir/terminal.clean" >/dev/null
grep -F '[usage]' "$tmpdir/terminal.clean" >/dev/null

c_terminal_root="$tmpdir/c-terminal-root"
write_terminal_fixture "$c_terminal_root"
{
  printf '%s\n' \
    "$tool_gate_prefix Use todo_kanban with arguments {\"operation\":\"add_item\",\"title\":\"CAI_MULTI_TURN_ALPHA\"}. Use the default board. After the tool result, answer TURN1_DONE." \
    "$tool_gate_prefix Use list_files with arguments {\"path\":\".\"}. After the tool result, answer TURN2_DONE and include note.txt if it is listed." \
    "$tool_gate_prefix Use read_file with arguments {\"path\":\"note.txt\"}. Do not set max_bytes, start_line, or end_line. After the tool result, answer TURN3_DONE and include NOTE_ALPHA if the file contains it." \
    "$tool_gate_prefix Use exec_command with arguments {\"command\":\"sh -s\",\"stdin\":\"printf EXEC_ALPHA; uname -s\\n\"}. Do not set shell or workdir. After the tool result, answer TURN4_DONE and include EXEC_ALPHA." \
    "$tool_gate_prefix Use todo_kanban with arguments {\"operation\":\"list_board\"}. Use the default board. After the tool result, answer TURN5_DONE and include CAI_MULTI_TURN_ALPHA if the item is present." \
    '/exit'
} >"$tmpdir/terminal-multiturn.input"
run_live_capture terminal-multiturn "$tmpdir/terminal-multiturn.out" env \
  CAI_TODO_STORE="$tmpdir/terminal-multiturn-todo.json" \
  CAI_TODO_LOCK="$tmpdir/terminal-multiturn-todo.lock" \
  "$terminal_chat_example" \
  --exec-tool-dir "$c_terminal_root" \
  --read-tool-dir "$c_terminal_root" \
  <"$tmpdir/terminal-multiturn.input"
strip_ansi "$tmpdir/terminal-multiturn.out" "$tmpdir/terminal-multiturn.clean"
assert_terminal_multi_turn_transcript "$tmpdir/terminal-multiturn.clean"

make lua-rock >/dev/null
eval "$(make -s lua-env)"
printf 'hello\n/exit\n' >"$tmpdir/lua-terminal.input"
run_live_capture lua-terminal "$tmpdir/lua-terminal.out" env \
  CAI_LUA_TODO_STORE="$tmpdir/lua-terminal-todo.json" \
  CAI_LUA_TODO_LOCK="$tmpdir/lua-terminal-todo.lock" \
  "$lua_executable" examples/lua-terminal-chat/main.lua \
  <"$tmpdir/lua-terminal.input"
strip_ansi "$tmpdir/lua-terminal.out" "$tmpdir/lua-terminal.clean"
grep -F 'hint: pass --exec-tool-dir <path> to enable exec_command rooted to that path' \
  "$tmpdir/lua-terminal.clean" >/dev/null
grep -F 'hint: pass --read-tool-dir <path> to enable' \
  "$tmpdir/lua-terminal.clean" >/dev/null
grep -F '> ' "$tmpdir/lua-terminal.clean" >/dev/null
grep -F '[response]' "$tmpdir/lua-terminal.clean" >/dev/null
grep -F '[usage]' "$tmpdir/lua-terminal.clean" >/dev/null

lua_terminal_root="$tmpdir/lua-terminal-root"
write_terminal_fixture "$lua_terminal_root"
{
  printf '%s\n' \
    "$tool_gate_prefix Use todo_kanban with arguments {\"operation\":\"add_item\",\"title\":\"CAI_MULTI_TURN_ALPHA\"}. Use the default board. After the tool result, answer TURN1_DONE." \
    "$tool_gate_prefix Use list_files with arguments {\"path\":\".\"}. After the tool result, answer TURN2_DONE and include note.txt if it is listed." \
    "$tool_gate_prefix Use read_file with arguments {\"path\":\"note.txt\"}. Do not set max_bytes, start_line, or end_line. After the tool result, answer TURN3_DONE and include NOTE_ALPHA if the file contains it." \
    "$tool_gate_prefix Use exec_command with arguments {\"command\":\"sh -s\",\"stdin\":\"printf EXEC_ALPHA; uname -s\\n\"}. Do not set shell or workdir. After the tool result, answer TURN4_DONE and include EXEC_ALPHA." \
    "$tool_gate_prefix Use todo_kanban with arguments {\"operation\":\"list_board\"}. Use the default board. After the tool result, answer TURN5_DONE and include CAI_MULTI_TURN_ALPHA if the item is present." \
    '/exit'
} >"$tmpdir/lua-terminal-multiturn.input"
run_live_capture lua-terminal-multiturn "$tmpdir/lua-terminal-multiturn.out" env \
  CAI_LUA_TODO_STORE="$tmpdir/lua-terminal-multiturn-todo.json" \
  CAI_LUA_TODO_LOCK="$tmpdir/lua-terminal-multiturn-todo.lock" \
  "$lua_executable" examples/lua-terminal-chat/main.lua \
  --exec-tool-dir "$lua_terminal_root" \
  --read-tool-dir "$lua_terminal_root" \
  <"$tmpdir/lua-terminal-multiturn.input"
strip_ansi "$tmpdir/lua-terminal-multiturn.out" "$tmpdir/lua-terminal-multiturn.clean"
assert_terminal_multi_turn_transcript "$tmpdir/lua-terminal-multiturn.clean"
