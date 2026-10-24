#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mode=${1:-host}
peer_start
plan '{"plan":["STARTUP ANSWER"]}'
run -Nni 'STARTUP USER'; ok
[[ $(stdout_text) == 'STARTUP ANSWER' ]]
banner='CAI Smith (C) 2026 C89 Systems AB https://c89.systems'
introduction='Enter sends your task.'
repo='https://github.com/sa6mwa/libcai/'
cai_intro=$'cai is an example of embedding libcai_app: https://github.com/sa6mwa/libcai/.\nEnter sends your task. /help for commands; cai -h for options.'
layout() {
  awk -v banner="$1" -v intro="$2" '
    BEGIN {count=split(intro,lines,"\n")}
    NR==1 && $0!=banner {exit 1}
    NR==2 && $0!="" {exit 1}
    NR>=3 && NR<3+count && $0!=lines[NR-2] {exit 1}
    NR==3+count && $0!="" {exit 1}
    END {if (NR<3+count) exit 1}' "$term/text" || fail 'Incorrect startup rows or margins'
}
absent "$root/stdout" "$banner"
absent "$root/stdout" "$introduction"
absent "$root/stdout" "$repo"
for utility in --help --version --list; do
  run "$utility"; ok
  absent "$root/stdout" "$banner"
  absent "$root/stdout" "$introduction"
  absent "$root/stdout" "$repo"
done
terminal_start
expect 'STARTUP USER'
if [[ "$mode" == cai ]]; then
  contains "$term/raw" $'\033[97mC\033[94mA\033[97mI\033[0m Smith (C) 2026 C89 Systems AB https://c89.systems'
  contains "$term/text" "$introduction"
  absent "$term/text" '**Enter**'
  contains "$term/text" "$repo"
  absent "$term/text" "<$repo>"
  absent "$term/text" '`libcai_app`'
  layout "$banner" "$cai_intro"
  contains "$term/text" '  STARTUP ANSWER'
  jq -Rse --arg banner "$banner" --arg intro "$introduction" \
    'index($banner) < index($intro) and index($intro) < index("> STARTUP USER")' "$term/text" >/dev/null
else
  absent "$term/text" "$banner"
  absent "$term/text" "$introduction"
  absent "$term/text" "$repo"
fi
terminal_close
if [[ "$mode" == cai ]]; then
  export NO_COLOR=1
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" "$banner"
  absent "$term/raw" $'\033[97mC\033[94mA\033[97mI'
  layout "$banner" "$cai_intro"
  terminal_close
  terminal_start -N
  layout "$banner" "$cai_intro"
  absent "$term/text" 'STARTUP USER'
  terminal_close
else
  export CAI_TEST_APP_BANNER=$'HOST STARTUP BANNER\n\n'
  export CAI_TEST_APP_INTRODUCTION='**HOST INTRODUCTION** with `rendered code`.'
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" 'HOST INTRODUCTION'
  contains "$term/text" 'rendered code'
  absent "$term/text" '**HOST INTRODUCTION**'
  layout 'HOST STARTUP BANNER' 'HOST INTRODUCTION with rendered code.'
  contains "$term/text" '  STARTUP ANSWER'
  jq -Rse 'index("HOST STARTUP BANNER") < index("HOST INTRODUCTION") and index("HOST INTRODUCTION") < index("> STARTUP USER")' "$term/text" >/dev/null
  terminal_close
  run --help; ok
  absent "$root/stdout" 'HOST STARTUP BANNER'
  absent "$root/stdout" 'HOST INTRODUCTION'
  export CAI_TEST_APP_BANNER='INLINE:' CAI_TEST_APP_INTRODUCTION_LEFT=3
  terminal_start
  expect 'STARTUP USER'
  [[ $(head -1 "$term/text") == 'INLINE:   HOST INTRODUCTION with rendered code.' ]]
  contains "$term/text" '  STARTUP ANSWER'
  terminal_close
  export CAI_TEST_APP_BANNER=$'CUSTOM MARGINS\n\n'
  export CAI_TEST_APP_INTRODUCTION_RIGHT=9
  export CAI_TEST_APP_INTRODUCTION='one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen'
  width=60
  terminal_start
  expect 'STARTUP USER'
  awk '
    NR==1 && $0!="CUSTOM MARGINS" {exit 1}
    NR==2 && $0!="" {exit 1}
    NR>=3 && /^$/ {exit lines>1 ? 0 : 1}
    NR>=3 {if (substr($0,1,3)!="   " || length($0)>51) exit 1; lines++}
  ' "$term/text" || fail 'Introduction did not honor both configured margins'
  contains "$term/text" '  STARTUP ANSWER'
  terminal_close
fi
