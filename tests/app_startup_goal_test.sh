#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export LOG_LEVEL=warn
for mode in clear replace pause stop replace_clear clear_replace same pause_resume error reject; do
  export CAI_TEST_STARTUP_GOAL_MODE="$mode"
  plan '{"plan":["__complete_goal__","HOST GOAL DONE"]}'
  before=$(requests)
  run -Nng 'INITIAL STARTUP GOAL'
  ok # The embedding probe validates the application's exit code itself.
  if [[ "$mode" == error ]]; then
    contains "$root/stderr" 'HOST STARTUP GOAL OBSERVER FAILED'
  fi
  case "$mode" in
    replace|clear_replace|same|pause_resume|reject)
      [[ $(requests) == $((before+2)) ]]
      contains "$root/stdout" 'HOST GOAL DONE'
      sed -n "$((before+1))p" "$root/peer/requests.jsonl" >"$root/request"
      expected='INITIAL STARTUP GOAL'
      if [[ "$mode" == replace || "$mode" == clear_replace ]]; then expected='HOST REPLACEMENT'; fi
      jq -e --arg expected "$expected" '[.input[]? | select(.role == "user") | .content[]? | .text?] | last | startswith("Continue working toward the active thread goal.") and contains("<objective>\n" + $expected + "\n</objective>") and contains("Completion audit:")' "$root/request" >/dev/null
      ;;
    *) [[ $(requests) == "$before" && ! -s "$root/stdout" ]] ;;
  esac
done
