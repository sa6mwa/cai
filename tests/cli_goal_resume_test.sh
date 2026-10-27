#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
for outcome in cancel failure; do
  for action in resume new; do
    count=$(requests)
    if [[ "$outcome" == cancel ]]; then
      ending='{"pause":true,"text":"STOP THIS TURN"}'
    else
      ending='{"error":"GOAL RESTART PROVIDER FAILURE"}'
    fi
    plan "$(jq -n --argjson chat "$chat" --argjson ending "$ending" '{chat:$chat,plan:[
      {tool:"create_goal",arguments:{objective:"Original restart objective"}},$ending
    ]}')"
    terminal_start -N
    send 'Create a goal and pursue it\r'
    if [[ "$outcome" == cancel ]]; then
      expect 'STREAM in flight'
      send '\033'; expect 'WARN: Turn stopped.'; expect 'goal=paused'
      release
    else
      expect 'GOAL RESTART PROVIDER FAILURE'
    fi
    plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
      {tool:"update_goal",arguments:{status:"complete"}},"DIRECT GOAL RESTART COMPLETED"
    ]}')"
    if [[ "$action" == resume ]]; then
      send '/goal resume\r'
      objective='Original restart objective'
    else
      send '/goal new Replacement restart objective\r'
      objective='Replacement restart objective'
    fi
    expect_turn "$send_offset" 'DIRECT GOAL RESTART COMPLETED'
    [[ $(requests) == "$((count+4))" ]]
    sed -n "$((count+3))p" "$root/peer/requests.jsonl" >"$root/restarted"
    contains "$root/restarted" "$objective"
    contains "$root/restarted" 'Completion audit:'
    send '/goal resume\r'; expect 'goal is not resumable'
    [[ $(requests) == "$((count+4))" ]]
    terminal_close
  done
done

# Reaffirming an active, busy goal must not create a duplicate turn or reset it.
count=$(requests)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[
  {tool:"create_goal",arguments:{objective:"Already pursuing this goal"}},
  {pause:true,text:"BUSY ACTIVE GOAL"},
  {tool:"update_goal",arguments:{status:"complete"}},"BUSY GOAL FINISHED"
]}')"
terminal_start -N
send 'Create and pursue the goal\r'; expect 'STREAM in flight'
send '/goal resume\r'; expect 'Goal control queued'
release
expect_turn "$send_offset" 'BUSY GOAL FINISHED'
[[ $(requests) == "$((count+4))" ]]
terminal_close
