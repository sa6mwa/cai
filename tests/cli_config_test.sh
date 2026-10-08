#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
base=("$cli" -C "$work")
config="$root/config/cai/config.ini"
run --help; ok; contains "$root/stdout" --config; [[ ! -e "$config" ]]
run --version; ok; [[ ! -e "$config" ]]
run --no-config --list; ok; [[ ! -e "$config" ]]
run --list; ok
[[ $(stat -c %a "$config") == 600 && $(stat -c %a "$(dirname "$config")") == 700 ]]
contains "$config" '# model = "gpt-6-luna"'
contains "$config" '# config_env_has_precedence = false'
contains "$config" '[env]'
absent "$config" '[response]'; absent "$config" '[session_limits]'
! grep -Ev '^[[:space:]]*$|^#|^\[' "$config"
peer_start
base=("$cli" -C "$work")
write_config() {
  cat >"$config" <<INI
config_env_has_precedence = ${1:-false}
[client]
provider = custom
base_url = "$endpoint"
api_key_env = CAI_API_KEY
[agent]
model = configured-model
reasoning_effort = high
terminal = false
image_generation = false
[env]
CAI_API_KEY = "config-key-secret"
INI
}
unset CAI_API_KEY
write_config
cp "$config" "$root/original"
plan '{"plan":["ConfigTurnOne"]}'
run -Nni first; ok; contains "$root/stdout" ConfigTurnOne
cmp "$config" "$root/original"
json "$root/peer/requests.jsonl" '.model == "configured-model" and .reasoning.effort == "high"'
absent "$root/stderr" config-key-secret
export CAI_API_KEY=normal-env-key
plan '{"plan":["EnvPriorityDone"]}'
run -Nni first; ok
tail -1 "$root/peer/headers.jsonl" >"$root/header"
json "$root/header" '.["Authorization"] == "Bearer normal-env-key"'
write_config true
plan '{"plan":["ConfigPriorityDone"]}'
run -Nni first; ok
tail -1 "$root/peer/headers.jsonl" >"$root/header"
json "$root/header" '.["Authorization"] == "Bearer config-key-secret"'
plan '{"plan":["CliOverridesDone","SavedSettingsDone"]}'
run -Nni explicit -m cli-model -r low; ok
run -ni resume; ok
tail -2 "$root/peer/requests.jsonl" >"$root/selected"
json "$root/selected" '.model == "cli-model" and .reasoning.effort == "low"'
write_config
printf 'LOG_LEVEL = warn\n' >>"$config"
plan '{"plan":["QuietFromConfig"]}'
run -Nni 'quiet configured logging'; ok; absent "$root/stderr" 'quiet configured logging'
export LOG_LEVEL=info
plan '{"plan":["NormalLogEnvironmentWins"]}'
run -Nni visible; ok
jq -se 'any(.[]; .lvl == "info" and .role == "user")' "$root/stderr" >/dev/null
unset LOG_LEVEL
before=$(requests)
for bad_config in '[agent]\nunknown_field=config-key-secret\n' '[env]\nCAI_API_KEY="config-key-secret\n' '[client]\ntimeout_ms=-9\n' '[client]\nprovider=""\n' '[agent]\npreset=smith-review\n' '[response]\nmax_output_tokens=100\n' '[runtime]\nrecord_transcript=false\n'; do
  printf '%b' "$bad_config" >"$config"
  run -Nni never; bad; contains "$root/stderr" config.ini:; absent "$root/stderr" config-key-secret
done
[[ $(requests) == "$before" ]]
write_config
cp "$config" "$root/alternate.ini"
printf '[paths]\nexport_dir = "exports"\n' >>"$root/alternate.ini"
plan '{"plan":["AlternateDone"]}'
run --config "$root/alternate.ini" -Nni alternate; ok
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
run --config "$root/alternate.ini" --export "$id"; ok
[[ -f "$root/exports/$id/$id.md" ]]
run --no-config --config "$root/alternate.ini" --list
[[ "$rc" == 2 ]]; contains "$root/stderr" 'mutually exclusive'
rm -rf "$root/strict"
(umask 222; run --config "$root/strict/config.ini" --list; ok)
[[ $(stat -c %a "$root/strict/config.ini") == 600 && $(stat -c %a "$root/strict") == 700 ]]
