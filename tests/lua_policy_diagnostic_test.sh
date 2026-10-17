#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runner=$cli
protocol=${1:-responses}
peer_start
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf 'LUA_SOURCE_MARKER\n' >"$work/relevant.c"
plan "{\"chat\":$chat,\"plan\":[{\"error\":\"Provider incident\",\"error_code\":\"cyber_policy\"},{\"tool\":\"read_file\",\"arguments\":{\"path\":\"relevant.c\"}},\"LUA_DIAGNOSTIC_EXPLANATION\"]}"
"$runner" "$support/lua/e2e_policy_diagnostic.lua" "$endpoint" "$protocol" "$work"
[[ $(requests) == 3 ]]
jq -s '.[-1]' "$root/peer/requests.jsonl" >"$root/diagnostic"
json "$root/diagnostic" '[.tools[]|(.name // .function.name)]|sort == ["list_files","read_file"]'
contains "$root/diagnostic" LUA_POLICY_ORIGINAL_TASK
contains "$root/diagnostic" LUA_SOURCE_MARKER
