#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# == 0 ]] || exit 2
exec "$root/scripts/release.sh" test-matrix
