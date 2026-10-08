#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# == 0 ]] || exit 2
"$root/scripts/release.sh" build-matrix
printf '%s\n' 'Cross build/link proof only: no cross runtime runner is configured.'
