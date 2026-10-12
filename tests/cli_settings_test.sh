#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
case "${1:?settings test requires responses or chat_completions}" in
  responses) chat=false ;;
  chat_completions) chat=true ;;
  *) fail 'Unknown settings test protocol' ;;
esac
peer_start
export OPENAI_API_KEY=fixture OPENROUTER_API_KEY=fixture CAI_TEST_API_BASE_URL="$endpoint"
printf 'settings tool output\n' >"$work/data.txt"
catalog='{"models":[{"slug":"gpt-6-luna","display_name":"Luna","context_window":272000,"supported_in_api":true,"default_reasoning_level":"medium","supported_reasoning_levels":[{"effort":"low"},{"effort":"medium"},{"effort":"high"},{"effort":"max"}]},{"slug":"gpt-6.1-sol","display_name":"Sol","context_window":272000,"supported_in_api":true,"default_reasoning_level":"medium","supported_reasoning_levels":[{"effort":"low"},{"effort":"medium"},{"effort":"high"},{"effort":"xhigh"},{"effort":"max"},{"effort":"ultra"}]},{"slug":"no-reasoning","supported_reasoning_levels":[]}]}'
settings_plan() { jq -n --argjson cat "$catalog" --argjson items "$1" --argjson chat "$chat" '{catalog:$cat,chat:$chat,plan:$items}' >"$root/plan"; plan "$(cat "$root/plan")"; }
selection() {
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  json "$root/request" ".model == \"$1\" and (.reasoning_effort // .reasoning.effort) == \"$2\""
  # Assert the actual outbound developer instructions for both wire protocols,
  # including queued mid-turn settings and restoration from encrypted history.
  if [[ "$1" == gpt-6.1-sol ]]; then
    contains "$root/request" 'Default to using clear, concise paragraphs'
  elif [[ "$1" == gpt-6-luna ]]; then
    absent "$root/request" 'Default to using clear, concise paragraphs'
  fi
  contains "$root/request" 'an agent based on GPT-6.'
  contains "$root/request" 'CAI has no functions.exec orchestration'
}
  if [[ "$chat" == true ]]; then export CAI_TEST_CHAT_COMPLETIONS=1; else unset CAI_TEST_CHAT_COMPLETIONS; fi
  base=("$cli" -p openai --no-terminal --no-image-generation -C "$work")
  settings_plan '[]'; terminal_start -N
  send '/help\r'; expect manual.; contains "$term/text" /reasoning; contains "$term/text" /model
  send '/reasoning\r'; expect max; expect 'Provider-advertised choices'
  settings_plan '["INITIAL settings response"]'; offset=$(wc -c <"$term/text"); send 'original settings conversation\r'; expect_turn "$offset" 'INITIAL settings response'; selection gpt-6-luna medium
  send '/model gpt-6.1-sol high\r'; expect 'Model changed to gpt-6.1-sol'
  send '/reasoning\r'; expect ultra
  send '/reasoning xhigh\r'; expect 'Reasoning effort changed to xhigh'
  send '/model low\r'; expect 'Reasoning effort changed to low'
  send '/model gpt-6-luna\r'; expect 'Model changed to gpt-6-luna'
  send '/reasoning xhigh\r'; expect 'does not advertise reasoning effort xhigh'
  send '/reasoning invalid\r'; expect 'unknown reasoning effort'
  send '/reasoning high extra\r'; expect 'use /reasoning EFFORT'
  send '/model gpt-6.1-sol high extra\r'; expect 'use /model MODEL'
  send '/model gpt-6.1-sol high\r'; expect 'reasoning effort high'
  settings_plan '[{"pause":true,"tool":"read_file","arguments":{"path":"data.txt"}},"BOUNDARY settings response"]'
  offset=$(wc -c <"$term/text"); send 'use a tool while switching settings\r'; expect 'STREAM in flight'; selection gpt-6.1-sol high
  count=$(requests)
  send '/model gpt-6-luna low\r'; expect 'Model gpt-6-luna; reasoning effort low queued'
  send '/reasoning high\r'; expect 'Reasoning effort high queued'
  send '/model\r'; expect 'Pending reasoning: high'; [[ $(requests) == "$count" ]]
  release; expect_turn "$offset" 'BOUNDARY settings response'; selection gpt-6-luna high
  contains "$root/request" 'settings tool output'; absent "$root/request" /reasoning
  send '/model gpt-6.1-sol ultra\r'; expect 'reasoning effort ultra'; terminal_close
  terminal_start; send '/model\r'; expect 'Reasoning effort: ultra'
  settings_plan '["RESUMED settings response"]'
  offset=$(wc -c <"$term/text"); send 'after resume\r'; expect_turn "$offset" 'RESUMED settings response'; selection gpt-6.1-sol ultra
  contains "$root/request" 'original settings conversation'
  send '/new\r'; expect '--- session ---'; send '/model\r'; expect 'Reasoning effort: medium'
  terminal_close
  terminal_start -N -m gpt-6-luna -r low
  send '/model no-reasoning\r'; expect 'Model changed to no-reasoning'
  send '/reasoning\r'; expect 'no reasoning-effort choices'
  send '/model unknown-model\r'; expect 'Model changed to unknown-model'
  send '/reasoning\r'; expect 'Model-specific reasoning choices are unavailable'
  send '/reasoning high\r'; expect 'Reasoning effort changed to high'
  terminal_close
  base=("$cli" -p openrouter --no-terminal --no-image-generation -C "$work")
  terminal_start -N
  count=$(wc -l <"$root/peer/gets")
  send '/model\r'; expect "OpenRouter's large catalog is not listed"
  [[ $(wc -l <"$root/peer/gets") == "$count" ]]; terminal_close
# The account catalog contract is protocol-independent and runs once.
if [[ "$chat" == true ]]; then exit 0; fi
# Account catalog has no UI filtering, even beyond row 50 or client visibility fields.
mkdir -p "$root/.codex"
printf '%s\n' '{"auth_mode":"chatgpt","tokens":{"id_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","access_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","refresh_token":"fixture","account_id":"fixture"}}' >"$root/.codex/auth.json"
unset CAI_TEST_CHAT_COMPLETIONS
jq -n '{plan:[],catalog:{models:([range(70)|{slug:("catalog-model-"+tostring),context_window:272000,supported_in_api:true}]+[{slug:"gpt-6.1-sol"},{slug:"future-account-only-model",supported_in_api:false,visibility:"hide",minimum_client_version:"999.0.0"}])}}' >"$root/catalog"
plan "$(cat "$root/catalog")"
base=("$cli" -p chatgpt --no-terminal --no-image-generation -C "$work")
terminal_start -N; send '/model\r'; expect future-account-only-model; expect catalog-model-69; expect gpt-6.1-sol
absent "$term/text" 'Showing 50'; terminal_close
contains "$root/peer/gets" 'client_version=0.160.0'
