#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/kanban-check.XXXXXX")
trap 'rm -rf "$root"' EXIT
LOG_LEVEL=error "$1" "$root"
