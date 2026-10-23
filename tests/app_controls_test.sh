#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export LOG_LEVEL=warn
only=${1:-all}
if [[ "$only" == all ]]; then
export CAI_TEST_APP_CONTROL=queue
plan '{"plan":["Initial reply","Host queued reply","Last host reply"]}'
run -Nni initial; ok
contains "$root/stdout" 'Last host reply'
contains "$root/peer/requests.jsonl" 'HOST NEXT PROMPT'
jq -se '[.[] | [.input[]? | select(.role=="user") | .content[]? | .text?] | last] == ["initial","HOST NEXT PROMPT","HOST LAST PROMPT"]' "$root/peer/requests.jsonl" >/dev/null
export CAI_TEST_APP_CONTROL=replay
plan '{"plan":["Resumed reply"]}'
run -ni resume; ok
contains "$root/stdout" 'Resumed reply'
export CAI_TEST_APP_CONTROL=replay_error
run -ni resume; bad
contains "$root/stderr" 'HOST REPLAY FAILED'
export CAI_TEST_APP_CONTROL=observer_error
plan '{"plan":["Observer failure"]}'
run -Nni initial; bad
contains "$root/stderr" 'HOST OBSERVER FAILED'
export CAI_TEST_APP_CONTROL=failure
plan '{"plan":[{"error":"provider exploded"}]}'
run -Nni initial; bad
contains "$root/stderr" 'provider exploded'
export CAI_TEST_APP_CONTROL=reject
plan '{"plan":["Completed turn"]}'
run -Nni initial; ok
fi
modes=(stop stopqueued startup_stop cancel cancelqueued steer overflow goals)
if [[ "$only" != all ]]; then modes=("$only"); fi
for mode in "${modes[@]}"; do
  export CAI_TEST_APP_CONTROL="$mode" CAI_TEST_APP_RELEASE="$root/peer/release"
  plan '{"plan":[{"pause":true,"text":"First reply"},"After control"]}'
  count=$(requests)
  run -Nni initial
  if [[ "$mode" == cancel ]]; then bad; else ok; fi
  if [[ "$mode" == stopqueued ]]; then [[ $(requests) == "$((count+1))" ]]; fi
  if [[ "$mode" == startup_stop ]]; then [[ $(requests) == "$count" ]]; fi
  if [[ "$mode" == cancelqueued ]]; then contains "$root/stdout" 'After control'; fi
done
if [[ "$only" == all ]]; then
  unset CAI_TEST_APP_RELEASE
  export CAI_TEST_APP_CONTROL=interactive
  plan '{"plan":["Initial interactive reply","Approved host reply"]}'
  terminal_start -N
  send 'initial\r'; expect 'approval'
  [[ $(requests) -ge 1 ]]
  count=$(requests)
  send '/status\r'; expect 'Metric'
  [[ $(requests) == "$count" ]]
  send '\033a'; expect 'Alt-Y approve'
  contains "$term/text" 'HOST NEXT PROMPT'
  send '\033e'; expect_prompt 'HOST NEXT PROMPT'
  send ' edited\r'; expect 'approval'
  send '\033a\033y'; expect 'Approved host reply'
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  contains "$root/request" 'HOST NEXT PROMPT edited'
  terminal_close
fi
