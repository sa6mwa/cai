#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
mkdir -p "$root/config/cai"
printf '[mcp.alpha]\nendpoint = %s/mcp/alpha\n' "${endpoint%/v1}" >"$root/config/cai/config.ini"
terminal_start -N
plan '{"mcp":{"alpha":{"call_result":{"content":[{"type":"text","text":"## Remote report\n\n**Rich MCP text**"}]}}},"plan":[{"tool":"read_file","arguments":{"path":"missing.file"}},{"tool":"mcp_5_alpha_echo","arguments":{"text":"report"}},"MARKDOWN TURN DONE"]}'
prompt 'exercise tool rendering' 'MARKDOWN TURN DONE'
contains "$term/text" '* Failed read_file'
contains "$term/text" '* MCP mcp_5_alpha_echo'
contains "$term/text" 'Rich MCP text'
absent "$term/text" '**Rich MCP text**'
absent "$term/text" '"content"'
plan '{"mcp":{"alpha":{"call_result":{"isError":true,"content":[{"type":"text","text":"Server **literal** failure"}]}}},"plan":[{"tool":"mcp_5_alpha_echo","arguments":{"text":"fail"}},"FAILURE TURN DONE"]}'
prompt 'exercise MCP failure' 'FAILURE TURN DONE'
contains "$term/text" '* MCP failed mcp_5_alpha_echo'
contains "$term/text" '> Server **literal** failure'
id=$(current_session_id)
terminal_close
before=$(requests)
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" '* Failed read_file'
contains "$term/text" '* MCP mcp_5_alpha_echo'
contains "$term/text" 'Rich MCP text'
absent "$term/text" '**Rich MCP text**'
contains "$term/text" '> Server **literal** failure'
absent "$term/text" '"content"'
terminal_close
[[ $(requests) == "$before" ]]
