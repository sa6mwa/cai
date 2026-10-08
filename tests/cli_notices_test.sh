#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
message='literal **stars** _path_ `tick` [name](url) café λ &amp;'
for n in {1..20}; do message+=" word_$n"; done
message+=' END_LITERAL'
export CAI_TEST_NOTICE_MESSAGE="$message"
for width in 40 72; do
  if [[ "$width" == 72 ]]; then export NO_COLOR=1; else unset NO_COLOR; fi
  terminal_start -N
  expect END_LITERAL
  contains "$term/text" '* WARN:'
  for literal in '**stars**' _path_ '`tick`' '[name](url)' 'café λ' '&amp;'; do contains "$term/text" "$literal"; done
  grep -Eq '^  word_' "$term/text"
  if [[ "$width" == 40 ]]; then contains "$term/raw" $'\033[91m*\033[0m'; else absent "$term/raw" $'\033[91m*\033[0m'; fi
  send '/goal\r'; expect 'No goal.'; expect 'manage one.'
  terminal_close
done
logs; contains "$root/logs.jsonl" END_LITERAL
