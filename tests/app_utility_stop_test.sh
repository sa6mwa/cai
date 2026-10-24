#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mkdir "$root/bin"
printf '#!/bin/sh\nexit 0\n' >"$root/bin/xdg-open"
chmod 700 "$root/bin/xdg-open"
export PATH="$root/bin:$PATH"
for mode in login_accept login_partial message_before message; do
  export CAI_TEST_UTILITY_STOP="$mode"
  if [[ "$mode" == message* ]]; then
    "$cli" -C "$work" -q utility-target -M 'Enqueue, then stop' >"$root/stdout" 2>"$root/stderr"
    if [[ "$mode" == message ]]; then
      jq -e '.status == "queued"' "$root/stdout" >/dev/null
    else
      [[ ! -s "$root/stdout" ]]
    fi
  else
    "$cli" -C "$work" --login >"$root/stdout" 2>"$root/stderr"
    contains "$root/stdout" 'Waiting for OAuth callback'
  fi
done
