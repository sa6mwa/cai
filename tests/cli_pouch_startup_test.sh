#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/pouch-startup-test.XXXXXX")
trap 'rm -rf "$root"' EXIT
"$1" "$root"
