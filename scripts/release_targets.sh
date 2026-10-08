#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
printf '%s\n' x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl
if "$root/scripts/osxcross_available.sh" --quiet; then
  printf '%s\n' arm64-apple-darwin
else
  status=$?
  [[ "$status" == 1 ]] || exit "$status"
fi
