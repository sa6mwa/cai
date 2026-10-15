#!/usr/bin/env bash
# Exercise real background index publication while the terminal is idle.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
terminal_start -N
for cycle in 1 2; do
  plan "{\"plan\":[\"IDLE CYCLE $cycle DONE\"]}"
  prompt "persist idle cycle $cycle" "IDLE CYCLE $cycle DONE"
  log=$(find "$root/cache/cai" -maxdepth 1 -name '*.log' -print -quit)
  [[ -n "$log" ]]
  offset=$(wc -c <"$log")
  flushed=0
  # liblockdc's default publication interval is ten seconds. Wait for its
  # completion event instead of sending input that could force a flush.
  for ((n=0;n<400;n++)); do
    [[ ! -s "$term/status" ]] || fail 'CAI exited while idle'
    if tail -c +$((offset+1)) "$log" |
        jq -se 'any(.[]; .msg == "index.flush" and .ns == "cai.sessions")' 2>/dev/null; then
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
