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
[[ $(grep -o '┌' "$term/feed" | wc -l) == 1 ]] || fail '/status split its metrics into multiple tables'
absent "$term/feed" 'Instruction files in context'
absent "$term/feed" '## Tools'
expect alpha; expect beta; expect missing_key
before=$(wc -l <"$root/peer/mcp_requests.jsonl")
send '/tools\r'; expect mcp_5_alpha_echo; expect mcp_4_beta_echo; expect mcp_5_alpha_raw
expect 'Accept raw text'; expect 'Echo | user text'; expect 'environment variable is unset'
absent "$term/text" fixture-mcp-secret
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == "$before" ]]
send 'pause a model turn\r'; expect 'STREAM in flight'
send '/tools\r'; expect mcp_4_beta_raw
release; expect 'INVENTORY TURN DONE'; expect 'Worked for'
# The transport invalidates its own catalog on this notification. Installed
# metadata must survive the next inventory and a new runtime in this process.
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,mcp:{alpha:{notify_tools_changed:true}},plan:[{tool:"mcp_5_alpha_echo",arguments:{text:"catalog invalidation"}},"NOTIFICATION TURN DONE"]}')"
prompt 'exercise metadata invalidation' 'NOTIFICATION TURN DONE'
send '/tools\r'; expect mcp_5_alpha_raw; expect 'Echo | user text'
send '/new\r'; expect 'Welcome to cai'
send '/tools\r'; expect mcp_5_alpha_echo; expect mcp_5_alpha_raw
terminal_close
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == $((before+1)) ]]
jq -se 'all(.[]; (.request.method != "initialize" or .server != "alpha") or .authorization == "Bearer fixture-mcp-secret")' "$root/peer/mcp_requests.jsonl" >/dev/null
jq -se 'all(.[]; .server != "disabled" and .server != "missing_key")' "$root/peer/mcp_requests.jsonl" >/dev/null
# Both providers can execute namespaced tools; only Responses uses raw input.
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:[{tool:"mcp_5_alpha_echo",arguments:{text:"hello MCP"}},{tool:"mcp_4_beta_raw",arguments:(if $chat then {payload:"raw \"text\"\nsecond line"} else "raw \"text\"\nsecond line" end)},"MCP EXECUTION DONE"]}')"
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

# These valid server/tool pairs previously both became mcp_a_b_c and stopped
# application startup. Each must be advertised and routed to its own server.
cat >"$config" <<INI
[client]
api_protocol = $protocol
[mcp.a_b]
endpoint = "$origin/mcp/a_b"
[mcp.a]
endpoint = "$origin/mcp/a"
[mcp.alpha]
endpoint = "$origin/mcp/alpha"
INI
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '
  {chat:$chat,mcp:{
    a_b:{tools:[{name:"c",inputSchema:{type:"object",properties:{text:{type:"string"}}}}]},
    a:{tools:[{name:"b_c",inputSchema:{type:"object",properties:{text:{type:"string"}}}}]}
  },plan:[{tool:"mcp_3_a_b_c",arguments:{text:"first destination"}},
          {tool:"mcp_1_a_b_c",arguments:{text:"second destination"}},
          {tool:"mcp_5_alpha_echo",arguments:{text:"unrelated tool"}},"UNIQUE MCP DONE"]}')"
run -Nni 'exercise distinct MCP aliases'; ok
[[ $(stdout_text) == 'UNIQUE MCP DONE' ]]
jq -se 'any(.[]; .server == "a_b" and .request.method == "tools/call" and .request.params.name == "c" and .request.params.arguments.text == "first destination") and
        any(.[]; .server == "a" and .request.method == "tools/call" and .request.params.name == "b_c" and .request.params.arguments.text == "second destination") and
        any(.[]; .server == "alpha" and .request.method == "tools/call" and .request.params.arguments.text == "unrelated tool")' "$root/peer/mcp_requests.jsonl" >/dev/null
tail -4 "$root/peer/requests.jsonl" | jq -se 'all(.[];
  [.tools[] | (.function.name // .name)] as $names |
  ($names | length) == ($names | unique | length) and
  ($names | index("mcp_3_a_b_c")) != null and
  ($names | index("mcp_1_a_b_c")) != null)' >/dev/null

# The new prefix counts toward the provider's limit. A 64-byte alias works;
# an overlong optional server warns without disabling the valid one.
printf -v max_name '%053d' 0
cat >"$config" <<INI
[client]
api_protocol = $protocol
[mcp.edge]
endpoint = "$origin/mcp/edge"
[mcp.over]
endpoint = "$origin/mcp/over"
INI
plan "$(jq -n --arg name "$max_name" --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '
  {chat:$chat,mcp:{
    edge:{tools:[{name:$name,inputSchema:{type:"object"}}]},
    over:{tools:[{name:($name+"x"),inputSchema:{type:"object"}}]}
  },plan:[{tool:("mcp_4_edge_"+$name),arguments:{}},"MCP NAME BOUNDARY DONE"]}')"
run -Nni 'exercise alias size limit'; ok
[[ $(stdout_text) == 'MCP NAME BOUNDARY DONE' ]]
contains "$root/stderr" 'fit 64 bytes including its server prefix'
tail -2 "$root/peer/requests.jsonl" | jq -se 'all(.[];
  [.tools[] | (.function.name // .name) | select(type == "string")] |
  all(.[]; length <= 64))' >/dev/null
jq -se 'any(.[]; .server == "edge" and .request.method == "tools/call") and
        all(.[]; .server != "over" or .request.method != "tools/call")' "$root/peer/mcp_requests.jsonl" >/dev/null
