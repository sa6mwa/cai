#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mode=${1:-cli}
base=("$cli" -C "$work")
unified="$root/unified"
run --home "$unified" --help; ok
[[ ! -e "$unified" ]]
contains "$root/stdout" '-c, --config'
run --home "$unified" --list; ok
[[ -f "$unified/config/config.ini" && -s "$unified/state/pouch.key" ]]
[[ ! -e "$root/config/cai" && ! -e "$root/state/cai" ]]
contains "$unified/config/config.ini" "# config_dir = \"$unified/config\""
contains "$unified/config/config.ini" "# runtime_dir = \"$unified/run\""
contains "$unified/config/config.ini" "# agents_md = \"$unified/config/AGENTS.md\""
contains "$unified/config/config.ini" "# export_dir = \"$unified/data/exports\""
[[ $(stat -c %a "$unified/config/config.ini") == 600 && $(stat -c %a "$unified/state") == 700 ]]
# An INI home value must not generate or load a second configuration file.
mkdir -m700 "$root/policies"
printf '[paths]\nhome_dir = "../ini-home"\n' >"$root/policies/chosen.ini"
run -c "$root/policies/chosen.ini" --list; ok
[[ -s "$root/ini-home/state/pouch.key" && ! -e "$root/ini-home/config/config.ini" ]]
(
  export CAI_HOME="$root/environment-home"
  run --list; ok
  [[ -f "$CAI_HOME/config/config.ini" ]]
  run -c "$root/policies/chosen.ini" --list; ok
  [[ -s "$CAI_HOME/state/pouch.key" ]]
  run --home "$root/cli-home" -c "$root/policies/chosen.ini" --state-dir "$root/override-state" --list; ok
  [[ -s "$root/override-state/pouch.key" && ! -e "$root/cli-home/state" ]]
)
run --home "$root/no-config" --no-config --list; ok
[[ -s "$root/no-config/state/pouch.key" && ! -e "$root/no-config/config" ]]
run -c "$root/policies/chosen.ini" --no-config --list; bad
contains "$root/stderr" 'mutually exclusive'
run --home ''; bad
run -c; bad
peer_start
terminal_start --home "$unified" -N
send '/help\r'; expect '/resume'
terminal_close
[[ $(requests) == 0 ]]
find "$unified/cache" -maxdepth 1 -name '*.log' -print -quit | grep -q .
[[ -d "$unified/state/pouch" && ! -e "$unified/state/history" ]]
[[ ! -e "$root/config/cai" && ! -e "$root/state/cai" && ! -e "$root/cache/cai" ]]
cp "$unified/config/config.ini" "$root/original.ini"
cp "$unified/state/pouch.key" "$root/original.key"
terminal_start --home "$unified" --temp --yes
terminal_close
cmp "$unified/config/config.ini" "$root/original.ini"
cmp "$unified/state/pouch.key" "$root/original.key"
[[ -z $(find "$unified/cache/temp" -mindepth 1 -maxdepth 1 -print -quit) ]]
if [[ "$mode" == host ]]; then
  export CAI_TEST_APP_HOME_DIRECTORY="$root/host-home"
  run --list; ok
  [[ -s "$root/host-home/state/pouch.key" && -f "$root/host-home/config/config.ini" ]]
  terminal_start -N
  send '/status\r'; expect '## Status'
  terminal_close
  find "$root/host-home/cache" -maxdepth 1 -name '*.log' -print -quit | grep -q .
fi
