#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_API_BASE_URL="$endpoint"
catalog='{"models":[{"slug":"gpt-5.6-luna","context_window":272000,"service_tiers":[{"id":"priority","name":"Fast"},{"id":"flex"}],"default_service_tier":"priority"}]}'
for chat in false true; do
  if [[ "$chat" == true ]]; then export CAI_TEST_CHAT_COMPLETIONS=1; fi
  plan "{\"chat\":$chat,\"catalog\":$catalog,\"plan\":[\"INITIAL\"]}"
  terminal_start -N
  send '/tier list\r'; expect 'provider routing'; expect priority; expect flex
  prompt 'initial request' INITIAL
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  json "$root/request" 'has("service_tier") | not'
  send '/fast\r'; expect 'Service tier selected: priority'
  send '/tier\r'; expect 'Service tier selected: flex'
  send '/tier unknown\r'; expect 'tier is not advertised'
  send '/tier list\r'; expect 'Selected: flex'
  send '/tier default\r'; expect 'Service tier selected: default'
  plan "{\"chat\":$chat,\"catalog\":$catalog,\"plan\":[{\"pause\":true,\"tool\":\"read_file\",\"arguments\":{\"path\":\"input\"}},\"CONTINUED\",\"FOLLOWUP DONE\"]}"
  printf 'TIER_TOOL_RESULT\n' >"$work/input"
  before=$(requests)
  send 'run tool\r'; expect 'STREAM in flight'
  send 'DEFERRED USER INPUT\r'; expect 'Q 1. DEFERRED USER INPUT'
  send '/fast\r'; expect 'Q 2. /fast'
  send '/tier list\r'; expect 'Selected: default'
  release; expect_assistant CONTINUED; expect_assistant 'FOLLOWUP DONE'
  expect 'Service tier selected: priority'
  absent "$term/feed" 'queued for the next safe boundary'
  expect_turn '' 'FOLLOWUP DONE'
  jq -s --argjson before "$before" '.[ $before: ]' "$root/peer/requests.jsonl" >"$root/turn"
  json "$root/turn" 'length == 3 and .[0].service_tier == "default" and .[1].service_tier == "priority" and .[2].service_tier == "priority" and (.[1] | tostring | contains("DEFERRED USER INPUT") | not) and (.[2] | tostring | contains("DEFERRED USER INPUT"))'
  # Explicit selection and steering use the same next-request boundary.
  for key in enter cycle steer; do
    plan "{\"chat\":$chat,\"catalog\":$catalog,\"plan\":[{\"pause\":true,\"tool\":\"read_file\",\"arguments\":{\"path\":\"input\"}},\"TIER $key CONTINUED\"]}"
    before=$(requests)
    send "routing $key\\r"; expect 'STREAM in flight'
    if [[ "$key" == enter ]]; then
      send '/tier flex\r'; expect 'Q 1. /tier flex'
      expected=flex
      previous=priority
    elif [[ "$key" == cycle ]]; then
      send '/tier\r'; expect 'Q 1. /tier'
      expected=default
      previous=flex
    else
      send '/fast\033\r'; expect 'S 1. /fast'
      expected=priority
      previous=default
    fi
    send '/tier list\r'; expect "Selected: $previous"
    release; expect_assistant "TIER $key CONTINUED"; expect 'Worked for'
    expect "Service tier selected: $expected"
    absent "$term/feed" 'queued for the next safe boundary'
    jq -s --argjson before "$before" '.[ $before: ]' "$root/peer/requests.jsonl" >"$root/turn"
    json "$root/turn" "length == 2 and .[0].service_tier == \"$previous\" and .[1].service_tier == \"$expected\""
  done
  terminal_close
  terminal_start
  send '/tier list\r'; expect 'Selected: priority'
  plan "{\"chat\":$chat,\"catalog\":$catalog,\"plan\":[\"RESUMED\"]}"
  prompt 'after resume' RESUMED
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  json "$root/request" '.service_tier == "priority"'
  send '/fast\r'; expect 'Service tier selected: default'
  send '/tier fast\r'; expect 'Service tier selected: priority'
  terminal_close
done
# Empty advertised lists permit normal routing but cannot enable Fast.
unset CAI_SERVICE_TIER
plan '{"chat":true,"catalog":{"models":[{"slug":"gpt-5.6-luna","service_tiers":[]}]},"plan":[]}'
terminal_start -N
send '/fast\r'; expect 'no available tier choices'
send '/tier priority\r'; expect 'tier is not advertised'
send '/tier default\r'; expect 'Service tier selected: default'
terminal_close
# Missing metadata is explicit, and provider-specific tier IDs remain usable.
plan '{"chat":true,"catalog":{"models":[]},"plan":["CUSTOM TIER"]}'
terminal_start -N
send '/tier list\r'; expect 'Tier metadata is unavailable'
send '/tier experimental\r'; expect 'Service tier selected: experimental'
prompt 'custom routing' 'CUSTOM TIER'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.service_tier == "experimental"'
terminal_close
# Config, environment, and explicit flags use the same routing surface.
printf '[agent]\nservice_tier = flex\n' >"$root/tier.ini"
plan '{"chat":true,"plan":["CONFIG","ENVIRONMENT","FLAG"]}'
for source in config env flag; do
  extra=(--config "$root/tier.ini")
  [[ "$source" != config ]] || unset CAI_SERVICE_TIER
  [[ "$source" == config ]] || export CAI_SERVICE_TIER=priority
  [[ "$source" != flag ]] || extra+=(--service-tier default)
  run -Nn "${extra[@]}" -i 'routing setting'; ok
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  case "$source" in config) value=flex ;; env) value=priority ;; flag) value=default ;; esac
  json "$root/request" ".service_tier == \"$value\""
done
unset CAI_SERVICE_TIER
mkdir -p "$root/.codex"
printf '%s\n' '{"auth_mode":"chatgpt","tokens":{"id_token":"fixture","access_token":"e30.eyJleHAiOiA0MTAyNDQ0ODAwfQ.fixture","refresh_token":"fixture","account_id":"fixture"}}' >"$root/.codex/auth.json"
unset CAI_TEST_CHAT_COMPLETIONS
plan '{"catalog":{"models":[]},"plan":["NORMAL CHATGPT"]}'
base=("$cli" -p chatgpt -m gpt-5.6-luna --no-terminal --no-image-generation -C "$work")
run -Nn --service-tier default -i 'normal ChatGPT routing'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" 'has("service_tier") | not'
