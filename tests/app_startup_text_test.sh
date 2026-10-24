#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mode=${1:-host}
peer_start
plan '{"plan":["STARTUP ANSWER"]}'
run -Nni 'STARTUP USER'; ok
[[ $(stdout_text) == 'STARTUP ANSWER' ]]
banner='CAI Smith (C) 2026 C89 Systems AB https://c89.systems'
introduction='Enter sends your task.'
absent "$root/stdout" "$banner"
absent "$root/stdout" "$introduction"
for utility in --help --version --list; do
  run "$utility"; ok
  absent "$root/stdout" "$banner"
  absent "$root/stdout" "$introduction"
done
terminal_start
expect 'STARTUP USER'
if [[ "$mode" == cai ]]; then
  contains "$term/raw" $'\033[97mC\033[94mA\033[97mI\033[0m Smith (C) 2026 C89 Systems AB https://c89.systems'
  contains "$term/text" "$introduction"
  absent "$term/text" '**Enter**'
  jq -Rse --arg banner "$banner" --arg intro "$introduction" \
    'index($banner) < index($intro) and index($intro) < index("> STARTUP USER")' "$term/text" >/dev/null
else
  absent "$term/text" "$banner"
  absent "$term/text" "$introduction"
fi
terminal_close
if [[ "$mode" == cai ]]; then
  export NO_COLOR=1
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" "$banner"
  absent "$term/raw" $'\033[97mC\033[94mA\033[97mI'
  terminal_close
else
  export CAI_TEST_APP_BANNER='HOST STARTUP BANNER'
  export CAI_TEST_APP_INTRODUCTION='**HOST INTRODUCTION** with `rendered code`.'
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" 'HOST INTRODUCTION'
  contains "$term/text" 'rendered code'
  absent "$term/text" '**HOST INTRODUCTION**'
  jq -Rse 'index("HOST STARTUP BANNER") < index("HOST INTRODUCTION") and index("HOST INTRODUCTION") < index("> STARTUP USER")' "$term/text" >/dev/null
  terminal_close
  run --help; ok
  absent "$root/stdout" 'HOST STARTUP BANNER'
  absent "$root/stdout" 'HOST INTRODUCTION'
fi
