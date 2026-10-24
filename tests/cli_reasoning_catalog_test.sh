#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_API_BASE_URL="$endpoint"
plan '{"catalog":{"models":[{"slug":"gpt-5.6-luna","supported_reasoning_levels":[{"effort":"medium"},{"effort":"high"}]}]},"plan":[{"tool":"set_reasoning","arguments":{"mode":"high"}},"CATALOG READY"]}'
terminal_start -N --reasoning-tool
contains "$root/config/cai/config.ini" '# reasoning_tool = false'
contains "$root/config/cai/config.ini" '# normal_reasoning_effort = "medium"'
contains "$root/config/cai/config.ini" '# high_reasoning_effort = "high"'
prompt complex 'CATALOG READY'
logs
absent "$root/logs.jsonl" cai.reasoning_tool.unavailable
jq -se 'map(.reasoning.effort) == ["medium","high"]' "$root/peer/requests.jsonl" >/dev/null
terminal_close
