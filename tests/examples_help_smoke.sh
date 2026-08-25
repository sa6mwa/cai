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
grep -F 'CAI_SMITH_ARGS=-v' <<<"$output" >/dev/null

smith_args=$(make -C examples --no-print-directory \
  --eval='print-smith-args: ; @printf "%s\n" "$(SMITH_TERMINAL_ARGS)"' \
  print-smith-args CAI_SMITH_ARGS=-v)
grep -Fx -- '-v' <<<"$smith_args" >/dev/null

if [[ -n "$terminal_chat" ]]; then
  terminal_output=$("$terminal_chat" --help 2>&1)
  grep -F "Uses CAI's ChatGPT subscription auth by default" \
    <<<"$terminal_output" >/dev/null
  missing_auth_root=$(mktemp -d)
  trap 'rm -rf "$missing_auth_root"' EXIT
  if env -u CAI_CHATGPT_AUTH_JSON XDG_STATE_HOME="$missing_auth_root" \
    "$terminal_chat" </dev/null >"$missing_auth_root/terminal.out" 2>&1; then
    printf 'terminal chat unexpectedly opened without CAI ChatGPT auth\n' >&2
    exit 1
  fi
  grep -F 'cai_chatgpt_auth_open failed:' "$missing_auth_root/terminal.out" \
    >/dev/null
  grep -F 'make chatgpt-login' "$missing_auth_root/terminal.out" >/dev/null
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
