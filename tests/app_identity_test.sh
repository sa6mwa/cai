#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/app-identity.XXXXXX")
trap 'rm -rf "$root"' EXIT
export HOME="$root" XDG_CONFIG_HOME="$root/config" XDG_STATE_HOME="$root/state" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
unset XDG_RUNTIME_DIR # This case asserts the cache fallback; the runtime matrix tests both roots.
"$1"
[[ -f "$root/config/smith/config.ini" && ! -e "$root/config/cai" && ! -e "$root/state/cai" ]]
grep -Fq 'namespace_prefix = "smith"' "$root/config/smith/config.ini"
grep -Fq 'queue_prefix = "smith"' "$root/config/smith/config.ini"
