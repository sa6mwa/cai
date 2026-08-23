#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:-.}
terminal_chat=${2:-}
chatgpt_login=${3:-}
smith_terminal=${4:-}

cd "$repo_root"
output=$(make -C examples help)

grep -F 'make run-basic-response' <<<"$output" >/dev/null
grep -F 'make run-chatgpt-login' <<<"$output" >/dev/null
grep -F 'make run-terminal-chat' <<<"$output" >/dev/null
grep -F 'make run-lua-chatgpt-login' <<<"$output" >/dev/null
grep -F 'make run-lua-terminal-chat' <<<"$output" >/dev/null
grep -F 'make run-smith-terminal' <<<"$output" >/dev/null
grep -F 'make run-lua-smith-terminal' <<<"$output" >/dev/null

if [[ -n "$terminal_chat" ]]; then
  terminal_output=$("$terminal_chat" --help 2>&1)
  grep -F 'gpt-5-nano with API keys and gpt-5.4-mini with ChatGPT auth' \
    <<<"$terminal_output" >/dev/null
fi

if [[ -n "$chatgpt_login" ]]; then
  login_output=$("$chatgpt_login" --help 2>&1)
  grep -F 'click or copy' <<<"$login_output" >/dev/null
  if grep -F -- '--browser-command' <<<"$login_output" >/dev/null; then
    printf 'chatgpt-login help must not offer an automatic browser opener\n' >&2
    exit 1
  fi
fi

if [[ -n "$smith_terminal" ]]; then
  smith_output=$("$smith_terminal" --help 2>&1)
  grep -F "Smith uses CAI's ChatGPT subscription auth by default" \
    <<<"$smith_output" >/dev/null
  grep -F 'Defaults to gpt-5.6-luna' <<<"$smith_output" >/dev/null
fi
