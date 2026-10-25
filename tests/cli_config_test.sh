#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
base=("$cli" -C "$work")
config="$root/config/cai/config.ini"
run --help; ok; contains "$root/stdout" --config; [[ ! -e "$config" ]]
run --version; ok; [[ ! -e "$config" ]]
run --no-config --list; ok; [[ ! -e "$config" ]]
(
  export CAI_CONFIG="$root/ignored.ini"
  run --no-config --list; ok; [[ ! -e "$CAI_CONFIG" && ! -e "$config" ]]
  printf '[agent]\ninvalid_field = true\n' >"$CAI_CONFIG"
  run --no-config --list; ok
  run --list; bad; contains "$root/stderr" ignored.ini
  : >"$root/override.ini"
  run --config "$root/override.ini" --list; ok
  run --config "$CAI_CONFIG" --no-config --list
  [[ "$rc" == 2 ]]; contains "$root/stderr" 'mutually exclusive'
)
# INI key paths are config-relative; CLI key paths are workspace-relative.
mkdir -m 700 "$root/key-config"
chmod 700 "$work"
printf '[store.default]\nkey_file = ini.key\n' >"$root/key-config/config.ini"
run --config "$root/key-config/config.ini" --lockd "pouch://$root/key-ini" --list; ok
[[ -s "$root/key-config/ini.key" && ! -e "$work/ini.key" ]]
run --config "$root/key-config/config.ini" --pouch-key-file selected.key --lockd "pouch://$root/key-cli" --list; ok
[[ -s "$work/selected.key" && ! -e "$root/key-config/selected.key" ]]
run --config "$root/key-config/config.ini" --pouch-key-file selected.key --lockd "pouch://$root/key-cli" --list; ok
run --config "$root/key-config/config.ini" --pouch-key-file '~/home.key' --lockd "pouch://$root/key-home" --list; ok
[[ -s "$root/home.key" ]]
run --no-config --pouch-key-file local.key --lockd "pouch://$root/key-local" --list; ok
[[ -s "$work/local.key" ]]
run --config "$root/key-config/config.ini" --pouch-key-file "$root/absolute.key" --lockd "pouch://$root/key-absolute" --list; ok
[[ -s "$root/absolute.key" ]]
run --list; ok
[[ $(stat -c %a "$config") == 600 && $(stat -c %a "$(dirname "$config")") == 700 ]]
contains "$config" '# model = "gpt-6-luna"'
contains "$config" '# config_env_has_precedence = false'
contains "$config" '## Configuration.'
contains "$config" "# config_dir = \"$root/config/cai\""
contains "$config" "# agents_md = \"$root/config/cai/AGENTS.md\""
contains "$config" "# skills_dir = \"$root/config/cai/skills\""
contains "$config" "# export_dir = \"$root/data/cai/exports\""
contains "$config" '# tool_output = "compact"'
contains "$config" '# todo_layout = "board"'
contains "$config" '# replay_turns = 20'
contains "$config" '[messaging]'
contains "$config" '# default_board = "default"'
contains "$config" '# enabled = false'
contains "$config" '# enable_auto_enqueue = false'
contains "$config" '# ack_timeout_ms = 300000'
contains "$config" '# queue_name = ""'
contains "$config" '## Empty checks ~/.codex/auth.json first, then encrypted application auth.'
contains "$config" '## Empty optionally loads config_dir/models.json;'
contains "$config" '## Remote lockd mTLS client PEM; empty supplies no client certificate.'
! grep -E '^# [^=]*$' "$config"
contains "$config" '[env]'
absent "$config" '[response]'; absent "$config" '[session_limits]'
! grep -Ev '^[[:space:]]*$|^#|^\[' "$config"
# Removing one comment marker from a path selects exactly the documented
# value; leaving it commented keeps automatic lookup on subsequent starts.
printf '[paths]\n' >"$root/uncommented.ini"
sed -n 's/^# \(config_dir\|agents_md\|skills_dir\|export_dir\) = /\1 = /p' "$config" >>"$root/uncommented.ini"
run --config "$root/uncommented.ini" --list; ok
run --config-dir "$root/custom-config" --list; ok
contains "$root/custom-config/config.ini" "# agents_md = \"$root/custom-config/AGENTS.md\""
contains "$root/custom-config/config.ini" "# skills_dir = \"$root/custom-config/skills\""
(
  unset XDG_CONFIG_HOME XDG_DATA_HOME
  run --config "$root/fallback.ini" --list; ok
  contains "$root/fallback.ini" "# agents_md = \"$root/.config/cai/AGENTS.md\""
  contains "$root/fallback.ini" "# export_dir = \"$root/.local/share/cai/exports\""
)
(
  export XDG_CONFIG_HOME=relative XDG_DATA_HOME=relative
  run --config "$root/relative-xdg.ini" --list; ok
  contains "$root/relative-xdg.ini" "# config_dir = \"$root/.config/cai\""
  contains "$root/relative-xdg.ini" "# export_dir = \"$root/.local/share/cai/exports\""
)
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
