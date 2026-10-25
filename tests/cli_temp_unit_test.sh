#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/temp-unit.XXXXXX")
trap 'rm -rf "$root"' EXIT
"$1" "$root"
[[ $(cat "$root/sentinel") == outside ]]
