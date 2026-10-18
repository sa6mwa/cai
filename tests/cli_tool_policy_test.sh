#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
config="$root/config/cai/config.ini"
mkdir -p "$(dirname "$config")"
cat >"$config" <<INI
[client]
api_protocol = $protocol
INI
turn() {
  plan "$(jq -n --argjson chat "$([[ $protocol == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:["POLICY DONE"]}')"
  run -ni 'inspect tool policy' "$@"; ok
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  jq -r '.tools[]? | .name // .function.name // .type' "$root/request" >"$root/tools"
}
turn -N
contains "$root/tools" chrome
if [[ $protocol == responses ]]; then contains "$root/tools" web_search; else absent "$root/tools" web_search; fi
for tool in todo_kanban searxng_search reverse_geocode; do absent "$root/tools" "$tool"; done
# Each existing capability can be removed without disabling unrelated tools.
turn --no-chrome --no-web-search --no-read-file --no-list-files --no-apply-patch --no-view-image --no-goals --no-skills --no-review-subagent
for tool in chrome web_search read_file list_files apply_patch view_image create_goal get_goal update_goal clear_goal run_review read_skill; do absent "$root/tools" "$tool"; done
turn --todo --searxng --revgeo
for tool in todo_kanban searxng_search reverse_geocode; do contains "$root/tools" "$tool"; done
# Flag/config/environment precedence is checked directly in
# cai_cli_runtime_options, without provider calls or encrypted session opens.
cat >>"$config" <<'INI'
[agent]
todo = true
web_search = false
[chrome]
enabled = false
INI
# Execute a real kanban operation and reopen the encrypted store on the next
# invocation. Only normal assistant final text belongs on stdout.
plan "$(jq -n --argjson chat "$([[ $protocol == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:[{tool:"todo_kanban",arguments:{operation:"add_item",title:"PERSISTENT TASK"}},"TASK ADDED"]}')"
run -ni 'add a task'; ok
[[ $(stdout_text) == 'TASK ADDED' ]]
plan "$(jq -n --argjson chat "$([[ $protocol == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:[{tool:"todo_kanban",arguments:{operation:"list_board"}},"TASK LISTED"]}')"
run -ni 'list tasks'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'PERSISTENT TASK'
[[ ! -e "$root/config/cai/todo.json" && ! -e "$root/config/cai/todo.lock" ]]
# Services are optional and lazy; a refused connection becomes a tool failure
# and the model can continue. Chrome startup needs no host registration.
plan "$(jq -n --argjson chat "$([[ $protocol == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:[{tool:"chrome",arguments:{action:"tabs"}},"BROWSER UNAVAILABLE"]}')"
run --chrome -ni 'inspect browser'; ok
contains "$root/stderr" 'No CAI Chrome host'
[[ $(stdout_text) == 'BROWSER UNAVAILABLE' ]]
# A subsequent normal turn remains usable after the failed browser operation.
turn
# Endpoint overrides reach the actual registered tools, not only the catalog.
chat=$([[ $protocol == chat_completions ]] && printf true || printf false)
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,catalog:{query:"fixture",results:[{title:"SEARCH RESULT",url:"https://example.test/",content:"SEARCH PAYLOAD"}]},plan:[{tool:"searxng_search",arguments:{query:"fixture"}},"SEARCH DONE"]}')"
run --searxng --searxng-url "$endpoint" -ni 'search'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'SEARCH PAYLOAD'
contains "$root/peer/gets" /v1/search
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,catalog:{display_name:"GEOCODE RESULT",address:{city:"Stockholm",country:"Sweden",country_code:"se"}},plan:[{tool:"reverse_geocode",arguments:{latitude:59.3,longitude:18.1}},"GEOCODE DONE"]}')"
run --revgeo --revgeo-url "$endpoint" -ni 'locate'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'GEOCODE RESULT'
contains "$root/peer/gets" /v1/reverse
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,catalog_status:503,plan:[{tool:"searxng_search",arguments:{query:"fixture"}},"SEARCH UNAVAILABLE"]}')"
run --searxng --searxng-url "$endpoint" -ni 'search unavailable service'; ok
contains "$root/stderr" tool_call_failed
[[ $(stdout_text) == 'SEARCH UNAVAILABLE' ]]
