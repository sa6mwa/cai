#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "$#" -ne 0 ]]; then
  printf 'usage: %s\n' "$0" >&2
  exit 2
fi

"$repo_root/scripts/cross_build.sh"
printf '%s\n' \
  'cross-test: no cross-target execution runner is configured; executable cross tests were not run.'
printf '%s\n' \
  'cross-test: use make package-verify for package-level target metadata and privacy verification.'
