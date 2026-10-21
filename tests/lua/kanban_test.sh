#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$3/lua-kanban.XXXXXX")
trap 'rm -rf "$root"' EXIT
LOG_LEVEL=error "$1" "$2" "$root"
