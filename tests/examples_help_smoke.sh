#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:-.}

cd "$repo_root"
output=$(make -C examples help)

grep -F 'make run-basic-response' <<<"$output" >/dev/null
grep -F 'make run-terminal-chat' <<<"$output" >/dev/null
grep -F 'make run-lua-terminal-chat' <<<"$output" >/dev/null

