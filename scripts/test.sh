#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
preset=${1:-debug}; shift "$(( $# > 0 ? 1 : 0 ))"
[[ "$preset" != host ]] || preset=release
exec ctest --preset "$preset" --no-tests=error --output-on-failure --stop-on-failure --parallel 1 "$@"
