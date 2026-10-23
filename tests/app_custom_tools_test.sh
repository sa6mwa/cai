#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
base=("$cli" --no-config -p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-terminal --no-image-generation -C "$work")
export CAI_TEST_APP_TOOLS=yes CAI_TEST_APP_TOOL_REPORT="$root/tools.json"
run --help; ok
json "$root/tools.json" '.registrations == 0 and .calls == 0'
run --list; ok
json "$root/tools.json" '.registrations == 0 and .calls == 0'
plan '{"plan":[{"tool":"host_fact","arguments":{"value":"host input"}},{"tool":"host_raw","arguments":"RAW_TOOL_INPUT"},"Custom tools done"]}'
run --messaging -Nni 'use host tools'; ok
contains "$root/stdout" 'Custom tools done'
json "$root/tools.json" '.registrations == 1 and .calls == 2'
jq -se 'all(.[]; any(.tools[]; .name == "host_fact") and any(.tools[]; .name == "host_raw") and any(.tools[]; .name == "agent_message"))' "$root/peer/requests.jsonl" >/dev/null
contains "$root/peer/requests.jsonl" HOST_TOOL_RESULT
# The copied hook runs again for a fresh runtime in each repeated invocation.
export CAI_TEST_APP_REPEAT=1
plan '{"plan":[{"tool":"host_fact","arguments":{}},"FIRST_DONE",{"tool":"host_fact","arguments":{}},"SECOND_DONE"]}'
run -Nni 'use the host tool'; ok
json "$root/tools.json" '.registrations == 2 and .calls == 2'
unset CAI_TEST_APP_REPEAT
# Registration failure and isolated review fail before any inference request.
export CAI_TEST_APP_TOOLS=reject
plan '{"plan":[]}'
: >"$root/peer/requests.jsonl"
run -Nni 'must not run'; [[ "$rc" == 1 ]]
json "$root/tools.json" '.registrations == 1 and .calls == 0'
[[ ! -s "$root/peer/requests.jsonl" ]]
export CAI_TEST_APP_TOOLS=yes
run --review; [[ "$rc" == 1 ]]
contains "$root/stderr" 'isolated review does not support host tools'
json "$root/tools.json" '.registrations == 0 and .calls == 0'
[[ ! -s "$root/peer/requests.jsonl" ]]
