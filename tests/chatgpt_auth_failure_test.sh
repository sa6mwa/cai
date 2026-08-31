#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
  printf 'usage: %s <c|lua|ownership-c|ownership-lua> <program> [script]\n' "$0" >&2
  exit 2
fi

mode=$1
program=$2
script=${3:-}
auth_json=$(mktemp "${TMPDIR:-/tmp}/cai-chatgpt-auth-test-XXXXXX")
output_file=$(mktemp "${TMPDIR:-/tmp}/cai-chatgpt-auth-output-XXXXXX")
home_dir=

cleanup() {
  rm -f "$auth_json" "$output_file"
  if [ -n "$home_dir" ]; then
    rm -rf "$home_dir"
  fi
}
trap cleanup EXIT HUP INT TERM

printf '%s\n' 'not valid json' >"$auth_json"

case "$mode" in
  c)
    if env -u OPENAI_API_KEY \
      CAI_CHATGPT_AUTH_JSON="$auth_json" \
      CAI_INTEGRATION_CHATGPT_E2E=1 \
      "$program" >"$output_file" 2>&1; then
      printf 'ChatGPT C probe unexpectedly accepted invalid auth JSON\n' >&2
      exit 1
    fi
    ;;
  lua)
    if [ -z "$script" ]; then
      printf 'Lua auth contract requires an e2e script\n' >&2
      exit 2
    fi
    if env -u OPENAI_API_KEY \
      CAI_CHATGPT_AUTH_JSON="$auth_json" \
      CAI_LUA_USAGE_LIMITS_E2E=1 \
      "$program" "$script" >"$output_file" 2>&1; then
      printf 'ChatGPT Lua probe unexpectedly accepted invalid auth JSON\n' >&2
      exit 1
    fi
    ;;
  ownership-c)
    home_dir=$(mktemp -d "${TMPDIR:-/tmp}/cai-chatgpt-auth-home-XXXXXX")
    mkdir "$home_dir/.codex"
    printf '%s\n' 'not CAI auth' >"$home_dir/.codex/auth.json"
    if env -u OPENAI_API_KEY \
      HOME="$home_dir" \
      XDG_STATE_HOME="$home_dir/state" \
      CAI_INTEGRATION_CHATGPT_E2E=1 \
      "$program" >"$output_file" 2>&1; then
      printf 'ChatGPT C probe unexpectedly ran without CAI auth\n' >&2
      exit 1
    fi
    if ! grep -F 'requires CAI auth' "$output_file" >/dev/null; then
      printf 'ChatGPT probe did not reject Codex auth fallback:\n' >&2
      cat "$output_file" >&2
      exit 1
    fi
    exit 0
    ;;
  ownership-lua)
    if [ -z "$script" ]; then
      printf 'Lua auth ownership contract requires an e2e script\n' >&2
      exit 2
    fi
    home_dir=$(mktemp -d "${TMPDIR:-/tmp}/cai-chatgpt-auth-home-XXXXXX")
    mkdir "$home_dir/.codex"
    printf '%s\n' 'not CAI auth' >"$home_dir/.codex/auth.json"
    if env -u OPENAI_API_KEY \
      HOME="$home_dir" \
      XDG_STATE_HOME="$home_dir/state" \
      CAI_LUA_USAGE_LIMITS_E2E=1 \
      "$program" "$script" >"$output_file" 2>&1; then
      printf 'ChatGPT Lua probe unexpectedly ran without CAI auth\n' >&2
      exit 1
    fi
    if ! grep -F 'CAI ChatGPT auth file is unavailable' "$output_file" >/dev/null; then
      printf 'ChatGPT Lua probe did not reject Codex auth fallback:\n' >&2
      cat "$output_file" >&2
      exit 1
    fi
    exit 0
    ;;
  *)
    printf 'unknown ChatGPT auth contract mode: %s\n' "$mode" >&2
    exit 2
    ;;
esac

if grep -F 'failed to parse auth JSON' "$output_file" >/dev/null ||
   grep -F 'root value must be an object' "$output_file" >/dev/null ||
   grep -F 'auth JSON does not contain ChatGPT tokens' "$output_file" >/dev/null; then
  exit 0
fi

printf 'ChatGPT auth failure did not reject the document before transport:\n' >&2
cat "$output_file" >&2
exit 1
