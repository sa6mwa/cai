#!/usr/bin/env bash
# Exercise real background index publication while the terminal is idle.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# Exercise the same real background publisher with a short interval in this
# disposable root. Keep durable writes, crypto and indexing enabled.
base+=(--lockd "pouch://$root/state/cai/pouch?indexer_flush_interval_seconds=1")
terminal_start -N
for cycle in 1 2; do
  plan "{\"plan\":[\"IDLE CYCLE $cycle DONE\"]}"
  log=$(find "$root/cache/cai" -maxdepth 1 -name '*.log' -print -quit)
  [[ -n "$log" ]]
  # Arm the observer before causing writes. A fast background flush can finish
  # before the terminal peer observes the turn-completed status redraw.
  offset=$(wc -c <"$log")
  prompt "persist idle cycle $cycle" "IDLE CYCLE $cycle DONE"
  flushed=0
  # The fixture's publication interval is one second. Wait for its
  # completion event instead of sending input that could force a flush.
  for ((n=0;n<400;n++)); do
    [[ ! -s "$term/status" ]] || fail 'CAI exited while idle'
    if tail -c +$((offset+1)) "$log" |
        jq -se 'any(.[]; .msg == "index.flush" and .ns == "cai.sessions")' >/dev/null 2>&1; then
      flushed=1
      break
    fi
    sleep .05
  done
  [[ "$flushed" == 1 ]] || fail 'No background session-index publication while idle'
  send '/status\r'; expect 'Reasoning effort'
done
terminal_close
terminal_start
expect 'IDLE CYCLE 2 DONE'
terminal_close
