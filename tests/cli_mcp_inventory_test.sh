#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
protocol=${1:-responses}
origin=${endpoint%/v1}
mkdir -p "$root/config/cai"
config="$root/config/cai/config.ini"
export MCP_FIXTURE_KEY=fixture-mcp-secret
export CAI_TEST_MCP_CONFIG_TRACE="$root/mcp-config"
cat >"$config" <<INI
[client]
api_protocol = $protocol
[mcp.alpha]
endpoint = "$origin/mcp/alpha"
api_key_env = MCP_FIXTURE_KEY
discovery_timeout_ms = 1000
[mcp.beta]
endpoint = "$origin/mcp/beta"
[mcp.disabled]
enabled = false
[mcp.missing_key]
endpoint = "$origin/mcp/missing_key"
api_key_env = MCP_UNSET_FIXTURE_KEY
INI
unset MCP_UNSET_FIXTURE_KEY
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:[{pause:true,text:"INVENTORY TURN DONE"}]}')"
terminal_start -N
contains "$root/mcp-config" "$origin/mcp/alpha timeout=30000 discovery=1000"
contains "$root/mcp-config" "$origin/mcp/beta timeout=30000 discovery=3000"
send '/status\r'; expect 'MCP servers'; expect 'Agent tools'
expect alpha; expect beta; expect missing_key
before=$(wc -l <"$root/peer/mcp_requests.jsonl")
send '/tools\r'; expect mcp_alpha_echo; expect mcp_beta_echo; expect mcp_alpha_raw
expect 'Accept raw text'; expect 'Echo | user text'; expect 'environment variable is unset'
absent "$term/text" fixture-mcp-secret
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == "$before" ]]
send 'pause a model turn\r'; expect 'STREAM in flight'
send '/tools\r'; expect mcp_beta_raw
release; expect 'INVENTORY TURN DONE'; expect 'Worked for'
# The transport invalidates its own catalog on this notification. Installed
# metadata must survive the next inventory and a new runtime in this process.
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,mcp:{alpha:{notify_tools_changed:true}},plan:[{tool:"mcp_alpha_echo",arguments:{text:"catalog invalidation"}},"NOTIFICATION TURN DONE"]}')"
prompt 'exercise metadata invalidation' 'NOTIFICATION TURN DONE'
send '/tools\r'; expect mcp_alpha_raw; expect 'Echo | user text'
send '/new\r'; expect 'Welcome to CAI'
send '/tools\r'; expect mcp_alpha_echo; expect mcp_alpha_raw
terminal_close
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == $((before+1)) ]]
jq -se 'all(.[]; (.request.method != "initialize" or .server != "alpha") or .authorization == "Bearer fixture-mcp-secret")' "$root/peer/mcp_requests.jsonl" >/dev/null
jq -se 'all(.[]; .server != "disabled" and .server != "missing_key")' "$root/peer/mcp_requests.jsonl" >/dev/null
# Both providers can execute namespaced tools; only Responses uses raw input.
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:[{tool:"mcp_alpha_echo",arguments:{text:"hello MCP"}},{tool:"mcp_beta_raw",arguments:(if $chat then {payload:"raw \"text\"\nsecond line"} else "raw \"text\"\nsecond line" end)},"MCP EXECUTION DONE"]}')"
run -Nni 'exercise MCP tools'; ok
[[ $(stdout_text) == 'MCP EXECUTION DONE' ]]
jq -se 'any(.[]; .server == "alpha" and .request.method == "tools/call" and .request.params.name == "echo" and .request.params.arguments.text == "hello MCP")' "$root/peer/mcp_requests.jsonl" >/dev/null
jq -se 'any(.[]; .server == "beta" and .request.method == "tools/call" and .request.params.name == "raw" and .request.params.arguments.payload == "raw \"text\"\nsecond line")' "$root/peer/mcp_requests.jsonl" >/dev/null
absent "$root/stderr" fixture-mcp-secret
before=$(wc -l <"$root/peer/mcp_requests.jsonl")
run --list; ok
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == "$before" ]]
run --no-mcp -Nni 'disabled MCP'; ok
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == "$before" ]]
# The trace above proves the production default is three seconds and that
# normal tool calls retain a separate timeout. Exercise the whole-discovery
# deadline with a short fixture budget instead of sleeping through that default.
cat >"$config" <<INI
[mcp.slow]
endpoint = "$origin/mcp/slow"
discovery_timeout_ms = 100
[mcp.broken]
endpoint = "$origin/mcp/broken"
[mcp.denied]
endpoint = "$origin/mcp/denied"
[mcp.empty]
endpoint = "$origin/mcp/empty"
INI
plan '{"mcp":{"slow":{"delay_ms":40},"broken":{"malformed":true},"denied":{"status":401},"empty":{"empty":true}},"plan":[]}'
terminal_start -N
send '/tools\r'; expect 'discovery timed out'; expect broken; expect denied; expect empty
expect 'read_file'
terminal_close
# Malformed server definitions fail clearly before any discovery.
printf '[mcp.invalid]\ndiscovery_timeout_ms = 0\n' >"$config"
run -Nni invalid; bad
contains "$root/stderr" 'requires an endpoint'
