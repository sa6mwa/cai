#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/app-check.XXXXXX")
trap 'rm -rf "$root"' EXIT
"$1" "$root" >"$root/stdout" 2>"$root/stderr" || {
  cat "$root/stderr" >&2
  exit 1
}
grep -Fq 'Usage: vectis -a smith -- [options]' "$root/stdout"
! grep -Fq BANNER_MUST_NOT_APPEAR_IN_HELP "$root/stdout"
! grep -Fq INTRODUCTION_MUST_NOT_APPEAR_IN_HELP "$root/stdout"
