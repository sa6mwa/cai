#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# A provider failure bounds the old, incorrect pursuit without waiting for a
# goal timeout. Correct validation must not send any request at all.
plan '{"plan":["__api_error__"]}'
printf '[agent]\ngoals = false\n' >"$root/no-goals.ini"
for mode in flag config env; do
  flags=()
  case "$mode" in
    flag) flags+=(--no-goals) ;;
    config) flags+=(--config "$root/no-goals.ini") ;;
    env) export CAI_GOALS=false ;;
  esac
  run "${flags[@]}" --goal 'Do not start'; bad
  contains "$root/stderr" 'require goal tools'
  run "${flags[@]}" --review-and-fix; bad
  contains "$root/stderr" 'require goal tools'
  [[ $(requests) == 0 ]]
  unset CAI_GOALS
done
terminal_start -N --no-goals
send '/goal new Do not start\r'; expect 'Goal pursuit requires goal tools'
send '/goal resume\r'; expect 'Goal pursuit requires goal tools'
send '/goal\r'; expect 'No goal.'
[[ $(requests) == 0 ]]
terminal_close
