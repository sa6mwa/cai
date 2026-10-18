#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runner=$cli
protocol=${1:-responses}
peer_start
mkdir -p "$root/.codex" "$root/config/cai" "$work/.git" "$work/nested"
printf 'LUA_CAI_GLOBAL\n' >"$root/config/cai/AGENTS.md"
printf 'LUA_CODEX_ORIGINAL\n' >"$root/.codex/AGENTS.md"
printf 'LUA_ANCESTOR\n' >"$work/AGENTS.md"
unset CODEX_HOME
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,usage:{prompt_tokens:10,completion_tokens:10,total_tokens:20},plan:["LUA_FIRST_DONE","LUA_RESUME_DONE","LUA_DISABLED_DONE"]}')"
"$runner" "$support/lua/e2e_context_instructions.lua" "$root" "$endpoint" "$protocol"
jq -s '.[0]' "$root/peer/requests.jsonl" >"$root/first"
jq -s '.[1]' "$root/peer/requests.jsonl" >"$root/second"
jq -s '.[2]' "$root/peer/requests.jsonl" >"$root/third"
contains "$root/first" LUA_CODEX_ORIGINAL
contains "$root/second" LUA_CODEX_UPDATED
contains "$root/second" '# AGENTS.md instructions from'
contains "$root/second" "current user's instructions and working context"
contains "$root/second" '<INSTRUCTIONS>'
contains "$root/second" '</INSTRUCTIONS>'
absent "$root/second" LUA_CODEX_ORIGINAL
absent "$root/third" LUA_CODEX_UPDATED
for file in first second third; do
  contains "$root/$file" LUA_CAI_GLOBAL
  contains "$root/$file" kept-lua-one
  contains "$root/$file" kept-lua-two
  absent "$root/$file" DROPPED_LUA_LINE
  absent "$root/$file" DROPPED_LUA_PREFIX
  jq -e '(.input // .messages) | tostring | contains("\u001b") | not' "$root/$file" >/dev/null
done
