#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# Use the real managed terminal, not the fixture's usual --no-terminal.
base+=(--terminal)
export CAI_TEST_YOLO_SECRET=YOLO_ENV_ALLOWED
export XDG_FUTURE_CAI_TEST='unchanged-xdg-value'
printf 'YOLO_OUTSIDE_FILE\n' >"$root/outside"
report=$(jq -c . "$support/fixtures/review.json")
command='printf "HOME=%s XDG=%s EXTRA=%s SECRET=%s" "$HOME" "$XDG_CONFIG_HOME" "$XDG_FUTURE_CAI_TEST" "${CAI_TEST_YOLO_SECRET-unset}"'
for mode in false true; do
  rm -f "$root/created"
  plan "$(jq -n --arg command "$command" '{plan:[
    {tool:"exec_command",arguments:{cmd:$command,yield_time_ms:1000}},
    {tool:"read_file",arguments:{path:"../outside"}},
    {tool:"apply_patch",arguments:"*** Begin Patch\n*** Add File: ../created\n+YOLO_CREATED\n*** End Patch\n"},
    "YOLO FINISHED"]}')"
  policy=--no-yolo; [[ "$mode" == false ]] || policy=--yolo
  before=$(requests)
  run -Nni 'Check execution policy' "$policy"; ok
  [[ $(stdout_text) == 'YOLO FINISHED' ]]
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  contains "$root/request" "HOME=$HOME"
  contains "$root/request" "XDG=$XDG_CONFIG_HOME"
  contains "$root/request" 'EXTRA=unchanged-xdg-value'
  if [[ "$mode" == true ]]; then
    contains "$root/request" 'SECRET=YOLO_ENV_ALLOWED'
    contains "$root/request" 'YOLO_OUTSIDE_FILE'
    [[ $(cat "$root/created") == YOLO_CREATED ]]
    contains "$root/stderr" 'YOLO enabled'
  else
    contains "$root/request" 'SECRET=unset'
    absent "$root/request" 'YOLO_OUTSIDE_FILE'
    [[ ! -e "$root/created" ]]
    contains "$root/stderr" 'Sandbox restrictions enabled'
  fi
done
# Runtime-created reviewer and the manual review child inherit the same policy.
plan "$(jq -n --arg report "$report" '{plan:[
  {tool:"run_review",arguments:{instructions:"Inspect the fixture"}},
  {tool:"read_file",arguments:{path:"../outside"}},$report,"CHILD DONE"]}')"
run -Nni 'Delegate the fixture review' --yolo; ok
tail -2 "$root/peer/requests.jsonl" >"$root/reviewer-requests"
contains "$root/reviewer-requests" 'YOLO_OUTSIDE_FILE'
plan "$(jq -n --arg report "$report" '{plan:[{tool:"read_file",arguments:{path:"../outside"}},$report]}')"
terminal_start -N --yolo
send '/review Inspect the fixture\r'; expect 'Fix parser'; expect 'Worked for'
tail -1 "$root/peer/requests.jsonl" >"$root/manual-review"
contains "$root/manual-review" 'YOLO_OUTSIDE_FILE'
send '/status\r'; expect 'YOLO (unrestricted)'
terminal_close
