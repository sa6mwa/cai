#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mkdir "$root/bin"
printf '#!/bin/sh\nexit 0\n' >"$root/bin/xdg-open"; chmod 700 "$root/bin/xdg-open"
export PATH="$root/bin:$PATH"
base=("$cli" --login)
for level in warn debug; do
  export LOG_LEVEL="$level" XDG_STATE_HOME="$root/state-$level"
  "${base[@]}" >"$root/stdout" 2>"$root/stderr" & login_pid=$!
  wait_for has "$root/stdout" 'Waiting for OAuth callback'
  url=$(grep -o 'http://localhost:[0-9]*/auth/callback' "$root/stdout")
  contains "$root/stdout" 'Open this URL to authenticate:'
  [[ $(curl -s -o /dev/null -w '%{http_code}' "${url%/auth/callback}/wrong") == 404 ]]
  [[ $(curl -s -o /dev/null -w '%{http_code}' "$url?error=access_denied") == 400 ]]
  rc=0; wait "$login_pid" || rc=$?; [[ "$rc" == 1 ]]
  jq -se 'any(.[]; .lvl == "error")' "$root/stderr" >/dev/null
  if [[ "$level" == warn ]]; then jq -se 'all(.[]; .lvl == "warn" or .lvl == "error" or .lvl == "fatal" or .lvl == "panic")' "$root/stderr" >/dev/null
  else jq -se 'any(.[]; .lvl == "debug")' "$root/stderr" >/dev/null; fi
  [[ ! -e "$XDG_STATE_HOME/cai/auth.json" ]]
done
