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
if [[ "$only" == edges ]]; then modes=(review_stop queue_goal close_ack); fi
if [[ "$only" == interactive_stop || "$only" == observer_interactive ]]; then modes=(); fi
for mode in "${modes[@]}"; do
  export CAI_TEST_APP_CONTROL="$mode" CAI_TEST_APP_RELEASE="$root/peer/release"
  plan '{"plan":[{"pause":true,"text":"First reply"},"After control"]}'
  flags=(-Nni initial)
  if [[ "$mode" == review_stop ]]; then
    report=$(jq -c . "$support/fixtures/review.json")
    plan "$(jq -n --arg report "$report" '{plan:[{pause:true,tool:"read_file",arguments:{path:"tracked.txt"}},$report]}')"
    printf 'review input\n' >"$work/tracked.txt"
    flags=(--review -i 'Review the fixture')
  elif [[ "$mode" == queue_goal ]]; then
    plan '{"plan":["Initial reply","__complete_goal__","Host goal done","__complete_goal__","Host goal done","__complete_goal__","Host goal done"]}'
  elif [[ "$mode" == close_ack ]]; then
    plan '{"plan":["Close acknowledgement probe"]}'
  elif [[ "$mode" == message_cancel || "$mode" == message_stop ]]; then
    plan '{"plan":[{"tool":"agent_message","arguments":{"operation":"send","recipient":"cai.cancel-target","text":"Cancel this pending delivery"}}]}'
    flags+=(--messaging)
  fi
  count=$(requests)
  run "${flags[@]}"
  if [[ "$mode" == cancel || "$mode" == message_cancel || "$mode" == close_ack ]]; then bad; else ok; fi
  if [[ "$mode" == stopqueued ]]; then [[ $(requests) == "$((count+1))" ]]; fi
  if [[ "$mode" == startup_stop ]]; then [[ $(requests) == "$count" ]]; fi
  if [[ "$mode" == cancelqueued ]]; then contains "$root/stdout" 'After control'; fi
  if [[ "$mode" == review_stop ]]; then [[ $(requests) == "$((count+1))" && ! -s "$root/stdout" ]]; fi
done
if [[ "$only" == edges || "$only" == interactive_stop || "$only" == observer_interactive ]]; then
  modes=(interactive_stop observer_interactive)
  if [[ "$only" != edges ]]; then modes=("$only"); fi
  unset CAI_TEST_APP_RELEASE
  for mode in "${modes[@]}"; do
    export CAI_TEST_APP_CONTROL="$mode"
    plan '{"plan":["Interactive reply","Must not request this"]}'
    count=$(requests)
    terminal_start -N --autoqueue
    send 'initial\r'
    wait_for test -s "$term/status"
    wait "$terminal_pid" || :
    terminal_pid=
    if [[ "$mode" == interactive_stop ]]; then
      [[ $(cat "$term/status") == 0 ]]
    else
      [[ $(cat "$term/status") == 1 ]]
      absent "$term/text" 'prompt input failed'
    fi
    [[ $(requests) == "$((count+1))" ]]
    exec 7>&-
    exec {terminal_event_fd}>&-
    terminal_events=
    unset CAI_TEST_TERMINAL_EVENT_FD
  done
fi
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
