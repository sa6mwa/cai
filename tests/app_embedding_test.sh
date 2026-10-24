#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_APP_BANNER=$'EMBEDDED_STARTUP_BANNER\n'
export CAI_TEST_APP_MODEL=host-configured-model
base=("$cli" --no-config -p custom --endpoint "$endpoint" --no-terminal --no-image-generation -C "$work")
run --help; ok
contains "$root/stdout" 'Usage: vectis -a smith -- [options]'
absent "$root/stdout" EMBEDDED_STARTUP_BANNER
[[ ! -e "$root/config/cai/config.ini" && ! -e "$root/state/cai" ]]
plan '{"plan":["HOST MODEL"]}'
run -Nni 'host configuration'; ok
json "$root/peer/requests.jsonl" '.model == "host-configured-model"'
absent "$root/stdout" EMBEDDED_STARTUP_BANNER
plan '{"plan":["CLI MODEL"]}'
run -Nni 'forwarded options' -m forwarded-model; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "forwarded-model"'
terminal_start -N
contains "$term/raw" EMBEDDED_STARTUP_BANNER
[[ $(grep -o EMBEDDED_STARTUP_BANNER "$term/raw" | wc -l) == 1 ]]
terminal_close
export CAI_TEST_APP_REPEAT=1
terminal_start -N
send '/quit\r'
two_banners() { [[ $(grep -o EMBEDDED_STARTUP_BANNER "$term/raw" | wc -l) == 2 ]]; }
wait_for two_banners
terminal_close
unset CAI_TEST_APP_REPEAT
export CAI_TEST_APP_LOG_PATH="$root/host.log"
export XDG_CACHE_HOME="$root/borrowed-cache"
export LOG_OUTPUT="$root/unused-env.log"
plan '{"plan":["BORROWED LOGGER TURN"]}'
terminal_start -N
prompt 'test host logger' 'BORROWED LOGGER TURN'
terminal_close
jq -se 'any(.[]; .role == "assistant" and .msg == "BORROWED LOGGER TURN") and any(.[]; .msg == "HOST_LOGGER_STILL_OPEN")' "$root/host.log" >/dev/null
[[ ! -e "$root/unused-env.log" ]]
[[ -z $(find "$root/borrowed-cache" -type f -name '*.log' -print) ]]
