#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/app-check.XXXXXX")
trap 'rm -rf "$root"' EXIT
export HOME="$root" XDG_CONFIG_HOME="$root/config" XDG_STATE_HOME="$root/state" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
"$1" "$root" >"$root/stdout" 2>"$root/stderr" || {
  cat "$root/stderr" >&2
  exit 1
}
grep -Fq 'Usage: vectis -a smith -- [options]' "$root/stdout"
! grep -Fq BANNER_MUST_NOT_APPEAR_IN_HELP "$root/stdout"
! grep -Fq INTRODUCTION_MUST_NOT_APPEAR_IN_HELP "$root/stdout"
