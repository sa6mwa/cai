#!/usr/bin/env bash
# Shell tests own commands and assertions; Python peers only supply wire data.
set -euo pipefail
set -E
trap 'printf "CLI assertion failed at %s:%s: %s\n" "${BASH_SOURCE[0]}" "$LINENO" "$BASH_COMMAND" >&2' ERR
cli=$(realpath "$1")
shift
support=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
root=$(mktemp -d "$(dirname "$cli")/cli-check.XXXXXX")
work="$root/work"
mkdir -p "$work"
peer_pid= terminal_pid=
cleanup() {
  rc=${1:-$?}
  trap - EXIT
  if [[ -n "$terminal_pid" ]]; then kill "$terminal_pid" 2>/dev/null || true; wait "$terminal_pid" 2>/dev/null || true; fi
  if [[ -n "$peer_pid" ]]; then kill "$peer_pid" 2>/dev/null || true; wait "$peer_pid" 2>/dev/null || true; fi
  if [[ "$rc" != 0 ]]; then find "$root" -name stderr -o -name text | while read -r file; do printf '%s\n' "$file" >&2; tail -80 "$file" >&2; done; printf 'Fixture retained: %s\n' "$root" >&2; else rm -rf "$root"; fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
while IFS='=' read -r name _; do case "$name" in CAI_TERMINAL_PEER|CAI_TEST_*) ;; LOG_*|HTTP_PROXY|HTTPS_PROXY|ALL_PROXY|http_proxy|https_proxy|all_proxy|NO_COLOR|OPENAI_API_KEY|OPENROUTER_API_KEY|BERGET_API_KEY|CAI_*) unset "$name" ;; esac; done < <(env)
export HOME="$root" XDG_CONFIG_HOME="$root/config" XDG_STATE_HOME="$root/state" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
export CAI_API_KEY=fixture NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost TERM=xterm-256color
export SHELL=/bin/sh
fail() { printf '%s\n' "$*" >&2; exit 1; }
contains() { grep -Fq -- "$2" "$1" || fail "Missing '$2' in $1"; }
absent() { if grep -Fq -- "$2" "$1"; then fail "Unexpected '$2' in $1"; fi; }
json() { jq -e "$2" "$1" >/dev/null || fail "JSON assertion failed: $2 ($1)"; }
wait_for() {
  local wait_deadline=$((SECONDS+10)) progress_byte
  while ((SECONDS < wait_deadline)); do
    if "$@"; then return; fi
    if [[ -n "${terminal_events:-}" ]]; then
      IFS= read -r -N 1 -t .05 -u "$terminal_event_fd" progress_byte || :
    else
      sleep .01
    fi
  done
  fail "Timed out: $*"
}
has() {
  [[ -f "$1" ]] || return 1
  grep -Fq -- "$2" "$1" ||
    { tr '\n\r\t' '   ' <"$1" | sed 's/[[:space:]][[:space:]]*/ /g' | grep -F -- "$2" >/dev/null; }
}
peer_start() {
  mkdir -p "$root/peer"
  printf '%s\n' '{"plan":[]}' >"$root/peer/config.json"
  "${CAI_TEST_PYTHON:-python3}" "$support/fixtures/http_peer.py" "$root/peer" >"$root/peer/stdout" 2>"$root/peer/stderr" & peer_pid=$!
  wait_for test -s "$root/peer/port"
  endpoint="http://127.0.0.1:$(cat "$root/peer/port")/v1"
  base=("$cli" -p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-terminal --no-image-generation -C "$work")
}
plan() {
  printf '%s\n' "$1" >"$root/peer/config.tmp"
  mv "$root/peer/config.tmp" "$root/peer/config.json"
  rm -f "$root/peer/release"
}
requests() { [[ -f "$root/peer/requests.jsonl" ]] && wc -l <"$root/peer/requests.jsonl" || printf '0\n'; }
release() { touch "$root/peer/release"; }
run() { rc=0; "${base[@]}" "$@" >"$root/stdout" 2>"$root/stderr" || rc=$?; }
ok() { [[ "$rc" == 0 ]] || fail "CLI exited $rc"; }
bad() { [[ "$rc" != 0 ]] || fail 'CLI unexpectedly succeeded'; }
stdout_text() { sed 's/^[[:space:]]*//;s/[[:space:]]*$//' "$root/stdout"; }
terminal_launch() {
  send_offset=0
  send_feed_offset=0
  term="$root/terminal-${terminal_sequence:-0}"
  terminal_sequence=$((${terminal_sequence:-0}+1))
  mkdir -p "$term"; mkfifo "$term/input" "$term/events"
  exec 7<>"$term/input"
  exec {terminal_event_fd}<>"$term/events"
  terminal_events=1
  export CAI_TEST_TERMINAL_EVENT_FD=$terminal_event_fd
  "$CAI_TERMINAL_PEER" "$term" "${width:-120}" "${base[@]}" "$@" <"$term/input" & terminal_pid=$!
}
terminal_start() { terminal_launch "$@"; wait_for has "$term/text" 'Send /help for usage.'; }
send() { send_offset=$(wc -c <"$term/text"); send_feed_offset=$(wc -c <"$term/feed"); printf '%b' "$1" >&7; }
has_since() {
  tail -c +$(($1+1)) "$term/text" | tr '\n\r\t' '   ' |
    sed 's/[[:space:]][[:space:]]*/ /g' | grep -F -- "$2" >/dev/null ||
    tail -c +$((${3:-0}+1)) "$term/feed" | tr '\n\r\t' '   ' |
    sed 's/[[:space:]][[:space:]]*/ /g' | grep -F -- "$2" >/dev/null
}
expect() { wait_for has_since "${send_offset:-0}" "$1" "${send_feed_offset:-0}"; }
expect_prompt() { wait_for has "$term/prompt" "$1"; }
assistant_logged() {
  logs
  jq -se --arg expected "$1" 'any(.[]; .role == "assistant" and .event == "assistant_text_delta" and .msg == $expected)' "$root/logs.jsonl" >/dev/null
}
# Fixture responses arrive as single deltas. Assert exact semantic content plus
# each rendered word; a sticky-line redraw may occur between feed writes.
expect_assistant() {
  local word
  local -a words
  read -r -a words <<<"$1"
  for word in "${words[@]}"; do expect "$word"; done
  wait_for assistant_logged "$1"
}
turn_finished() {
  tail -c +$((${3:-0}+1)) "$term/feed" | tr '\n\r\t' '   ' |
    sed 's/[[:space:]][[:space:]]*/ /g' | grep -F -- "$2" >/dev/null || return 1
  tail -c +$(($1+1)) "$term/text" | awk -v marker="$2" '
    BEGIN {count=split(marker,words,/[[:space:]]+/); word=1}
    {while(word<=count) {position=index($0,words[word]); if(!position) break; $0=substr($0,position+length(words[word])); word++}}
    word>count && index($0,"Worked for") {done=1}
    END {exit !done}'
}
expect_turn() { wait_for turn_finished "$1" "$2" "${send_feed_offset:-0}"; }
prompt() { local offset=$(wc -c <"$term/text"); send "$1\r"; expect_turn "$offset" "$2"; }
resize() { printf '%s\n' "$1" >"$term/width"; kill -USR1 "$terminal_pid"; }
terminal_close() {
  send '\025/quit\r'
  wait_for test -s "$term/status"
  wait "$terminal_pid" || fail 'Terminal CLI failed'
  terminal_pid=
  [[ $(cat "$term/status") == 0 ]] || fail 'Nonzero terminal exit'
  [[ ! -s "$term/stderr" ]] || fail 'Unexpected terminal stderr'
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD
}
logs() { find "$root/cache/cai" -maxdepth 1 -name '*.log' -exec cat {} + >"$root/logs.jsonl"; }
