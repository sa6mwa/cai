#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
export CAI_TEST_APP_NAME=smith SMITH_API_KEY=fixture SMITH_MODEL=smith-model
peer_start
base=("$cli" -p custom --endpoint "$endpoint" --no-terminal --no-image-generation -C "$work")
run --help; ok
contains "$root/stdout" 'smith and libcai Copyright'
contains "$root/stdout" 'Usage: vectis -a smith --'
absent "$root/stdout" CAI_API_KEY
contains "$root/stdout" SMITH_API_KEY
[[ ! -e "$root/config/smith" && ! -e "$root/state/smith" ]]
mkdir -p "$root/config/smith" "$root/skill-source"
printf 'SMITH_GLOBAL_INSTRUCTIONS_MARKER\n' >"$root/config/smith/AGENTS.md"
printf '%s\n' '---' 'name: identity-test' 'description: Named application skill marker' '---' 'Test skill instructions.' >"$root/skill-source/SKILL.md"
run --install-skill "$root/skill-source"; ok
[[ -f "$root/config/smith/skills/identity-test/SKILL.md" ]]
plan '{"plan":["NAMED APP REPLY"]}'
run -Nni 'named application'; ok
json "$root/peer/requests.jsonl" '.model == "smith-model"'
contains "$root/stdout" 'NAMED APP REPLY'
contains "$root/peer/requests.jsonl" SMITH_GLOBAL_INSTRUCTIONS_MARKER
contains "$root/peer/requests.jsonl" 'Named application skill marker'
run --list; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
[[ -n "$id" && -f "$root/config/smith/config.ini" && -f "$root/state/smith/pouch.key" ]]
run --export "$id"; ok
[[ -s "$root/data/smith/exports/$id/$id.md" && -s "$root/data/smith/exports/$id/$id.jsonl" ]]
terminal_start --resume "$id"
send '/queue on\r'; expect 'smith.work'
send '/status\r'; expect 'smith global AGENTS.md'
terminal_close
[[ -d "$root/cache/smith/clipboard" && -d "$root/state/smith/history" ]]
find "$root/cache/smith" -name '*.log' | grep -q .
[[ ! -e "$root/config/cai" && ! -e "$root/state/cai" && ! -e "$root/cache/cai" && ! -e "$root/data/cai" ]]
# The embedding host can choose a different layout without changing identity.
export CAI_TEST_APP_CONFIG_FILE="$root/config/vectis/cai.ini"
export CAI_TEST_APP_CONFIG_DIRECTORY="$root/config/vectis/instructions"
export CAI_TEST_APP_STATE_DIRECTORY="$root/state/vectis/cai"
export CAI_TEST_APP_CACHE_DIRECTORY="$root/cache/vectis/cai"
export CAI_TEST_APP_DATA_DIRECTORY="$root/data/vectis/cai"
export CAI_TEST_APP_NAMESPACE_PREFIX=vectis.smith CAI_TEST_APP_QUEUE_PREFIX=vectis
mkdir -p "$root/config/vectis/instructions"
printf 'VECTIS_GLOBAL_INSTRUCTIONS_MARKER\n' >"$root/config/vectis/instructions/AGENTS.md"
plan '{"plan":["OVERRIDDEN APP REPLY"]}'
run -Nni 'overridden layout'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" VECTIS_GLOBAL_INSTRUCTIONS_MARKER
absent "$root/request" SMITH_GLOBAL_INSTRUCTIONS_MARKER
[[ -f "$root/config/vectis/cai.ini" && -f "$root/state/vectis/cai/pouch.key" ]]
contains "$root/config/vectis/cai.ini" "$root/config/vectis/instructions/AGENTS.md"
run --list; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
run --export "$id"; ok
[[ -s "$root/data/vectis/cai/exports/$id/$id.md" ]]
# An explicit env prefix does not fall back to SMITH_ or CAI_.
export CAI_TEST_APP_ENVIRONMENT_PREFIX=BOT_ BOT_API_KEY=fixture BOT_MODEL=bot-model
plan '{"plan":["PREFIX OVERRIDE"]}'
run -Nni 'different environment prefix'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "bot-model"'
run --help; ok
contains "$root/stdout" BOT_API_KEY
absent "$root/stdout" SMITH_API_KEY
# Every root and namespace can also be overridden by forwarded flags.
base+=(--state-dir "$root/flags/state" --cache-dir "$root/flags/cache"
       --data-dir "$root/flags/data" --history-dir "$root/flags/history"
       --clipboard-dir "$root/flags/clipboard" --log-dir "$root/flags/logs"
       --pouch-key-file "$root/flags/key" --namespace-prefix flags
       --sessions-namespace flags.sessions-explicit --scopes-namespace flags.scopes-explicit
       --auth-namespace flags.auth-explicit --todo-namespace flags.todo-explicit
       --messages-namespace flags.messages-explicit --queue-prefix dispatch)
plan '{"plan":["FLAG OVERRIDES"]}'
run -Nni 'forwarded roots'; ok
[[ -f "$root/flags/key" && -d "$root/flags/state/pouch" && -d "$root/flags/clipboard" ]]
run --list; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
run --export "$id"; ok
[[ -s "$root/flags/data/exports/$id/$id.md" ]]
terminal_start --resume "$id"
send '/queue on\r'; expect 'dispatch.work'
terminal_close
[[ -d "$root/flags/history" ]]
find "$root/flags/logs" -name '*.log' | grep -q .
export BOT_TODO=invalid
run --list
[[ $rc == 2 ]]
contains "$root/stderr" BOT_TODO
unset BOT_TODO
