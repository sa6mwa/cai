#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/app-home.XXXXXX")
trap 'rm -rf "$root"' EXIT
export HOME="$root" XDG_CONFIG_HOME="$root/config" XDG_STATE_HOME="$root/state" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
unset CAI_HOME CAI_PATHS_HOME_DIR CAI_CONFIG CAI_CONFIG_DIR CAI_CACHE_DIR CAI_STATE_DIR SMITH_HOME HOST_HOME
export XDG_RUNTIME_DIR="$root/xdg-runtime"
mkdir -p "$root/work"
cd "$root/work"
"$1"
[[ -f "$root/work/unified/config/config.ini" && ! -e "$root/work/ini-home/config/config.ini" ]]
