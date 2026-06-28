#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=${script_dir%/scripts}
compose_file="$repo_root/docker-compose.yaml"

if command -v nerdctl >/dev/null 2>&1; then
  exec nerdctl compose -f "$compose_file" "$@"
fi

if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
  exec docker compose -f "$compose_file" "$@"
fi

printf '%s\n' "Neither nerdctl nor docker compose is available in PATH." >&2
exit 1
