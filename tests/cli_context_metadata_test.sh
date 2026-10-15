#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_API_BASE_URL="$endpoint"
base+=( -m gpt-6-luna )
catalog='{"models":[{"slug":"gpt-6-luna","context_window":272000,"auto_compact_token_limit":217600}]}'
plan "$(jq -n --argjson catalog "$catalog" '{catalog:$catalog,plan:["ContextFirstDone"]}')"
terminal_start -N
send '/status\r'; expect 'Status'
absent "$term/feed" '1050000 tokens'
absent "$term/feed" '1000000 tokens'
[[ $(requests) == 0 ]]
prompt 'measure effective context' ContextFirstDone
send '/status\r'; expect '272000 tokens'
terminal_close
# Offline startup must retain the selected model's last effective window,
# before another turn or a catalog request is made.
plan '{"catalog_status":503,"plan":[]}'
terminal_start
send '/status\r'; expect '272000 tokens'
absent "$term/feed" '1050000 tokens'
terminal_close
# A real provider update supersedes saved metadata at the next safe boundary.
catalog='{"models":[{"slug":"gpt-6-luna","context_window":1050000,"auto_compact_token_limit":840000}]}'
plan "$(jq -n --argjson catalog "$catalog" '{catalog:$catalog,plan:["ContextUpdatedDone"]}')"
terminal_start
prompt 'refresh provider metadata' ContextUpdatedDone
send '/status\r'; expect '1050000 tokens'
terminal_close
