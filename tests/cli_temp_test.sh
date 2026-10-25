#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
controls=$(realpath "$1")
scope=${2:-mock}
base=("$cli")
for args in '--temp --list' '--temp --resume old' '--force' '--replace'; do
  read -r -a flags <<<"$args"
  run "${flags[@]}"; bad
done
[[ ! -e "$root/cache/cai/temp" ]]
peer_start
config="$root/config/custom-agent.ini"
cat >"$config" <<INI
## Preserve this exact configuration, including comments.
[client]
provider = custom
base_url = "$endpoint"
api_key_env = CAI_API_KEY
[agent]
model = config-temp-model
image_generation = false
[store.sessions]
endpoint = pouch://$root/normal-pouch
key_file = $root/normal.key
[store.messages]
endpoint = pouch://$root/message-pouch
key_file = $root/message.key
[storage]
sessions_store = sessions
messaging_store = messages
INI
cp "$config" "$root/config-original"
base=("$cli" --config "$config" --no-terminal -C "$work")
run --list; ok; cp "$root/stdout" "$root/list-before"
temp_workspace() { jq -sr '[.[] | select(.event == "temp_workspace_opened") | .workspace] | last' "$root/stderr"; }
write_plan() {
  plan '{"plan":[{"tool":"apply_patch","arguments":"*** Begin Patch\n*** Add File: kept.txt\n+TEMP CONTENT\n*** End Patch"},"TEMP DONE"]}'
}
if [[ "$scope" == host ]]; then
  # Verify each facade's argv and programmatic settings boundary. Cleanup,
  # messaging, auth, and control failure modes run once in the full CLI scope.
  for setting in argv receiver; do
    flags=(--temp)
    if [[ "$setting" == receiver ]]; then
      export CAI_TEST_APP_TEMPORARY=true
      flags=(--force)
    fi
    plan '{"plan":["HOST TEMP DONE"]}'
    run "${flags[@]}" -ni 'temporary mode from host'; ok
    [[ $(stdout_text) == 'HOST TEMP DONE' ]]
    workspace=$(temp_workspace)
    [[ "$workspace" == "$root/cache/cai/temp/"*/workspace ]]
    [[ ! -e "${workspace%/workspace}/private" ]]
    if [[ "$setting" == argv ]]; then
      [[ -d "$workspace" ]]
    else
      [[ ! -e "${workspace%/workspace}" ]]
    fi
    cmp "$config" "$root/config-original"
    unset CAI_TEST_APP_TEMPORARY
    run --list; ok; cmp "$root/stdout" "$root/list-before"
  done
  exit 0
fi
for option in '' --yes --force; do
  write_plan
  run --temp -ni 'write the temporary file' ${option:+"$option"}; ok
  [[ $(stdout_text) == 'TEMP DONE' ]]
  workspace=$(temp_workspace)
  [[ "$workspace" == "$root/cache/cai/temp/"*/workspace ]]
  private="${workspace%/workspace}/private"
  [[ ! -e "$private" ]]
  if [[ -z "$option" ]]; then
    [[ $(cat "$workspace/kept.txt") == 'TEMP CONTENT' ]]
    json "$root/stderr" 'select(.event == "temp_workspace_kept") | .workspace != null'
  else
    [[ ! -e "${workspace%/workspace}" ]]
  fi
  cmp "$config" "$root/config-original"
  run --list; ok; cmp "$root/stdout" "$root/list-before"
done
contains "$root/peer/requests.jsonl" config-temp-model
[[ ! -e "$work/kept.txt" ]]
[[ ! -e "$root/state/cai/history" ]]
plan '{"plan":["__api_error__"]}'
run --temp --force -ni 'provider failure'; bad
workspace=$(temp_workspace)
[[ ! -e "${workspace%/workspace}" ]]

# The shutdown question is plain terminal input/output after softline restores
# the terminal; redirecting diagnostic stderr must not hide it.
for answer in n '' yes; do
  plan '{"plan":[]}'
  terminal_start --temp
  workspace=$(find "$root/cache/cai/temp" -path '*/private/state/pouch.key' -printf '%h\n' | sed 's,/private/state$,/workspace,')
  [[ -d "$workspace" ]]
  [[ -d "${workspace%/workspace}/private/state/history" ]]
  printf 'interactive file\n' >"$workspace/kept.txt"
  send '/quit\r'
  wait_for has "$term/text" '[y/N]'
  [[ ! -e "$term/status" ]]
  printf '%s\n' "$answer" >&7
  wait_for test -s "$term/status"
  wait "$terminal_pid"; terminal_pid=
  [[ $(cat "$term/status") == 0 && ! -e "${workspace%/workspace}/private" ]]
  if [[ "$answer" == yes ]]; then
    [[ ! -e "${workspace%/workspace}" ]]
  else
    [[ $(cat "$workspace/kept.txt") == 'interactive file' ]]
    contains "$term/text" 'Kept temporary workspace:'
  fi
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
done

# A normal receiver and a temporary sender use the explicitly selected shared
# messaging store, independently of private and named conversation stores.
plan '{"plan":[]}'
terminal_start --messaging --message-queue cai.normal
plan '{"plan":[{"tool":"agent_message","arguments":{"operation":"send","recipient":"cai.normal","text":"TEMP TO NORMAL"}},"MESSAGE SENT"]}'
run --temp --force --messaging -ni 'send to normal'; ok
contains "$root/stdout" 'MESSAGE SENT'
expect 'TEMP TO NORMAL'
terminal_close
terminal_start --temp --force --messaging --message-queue cai.temporary
run -q cai.temporary -M 'NORMAL TO TEMP'; ok
expect 'NORMAL TO TEMP'
terminal_close
cmp "$config" "$root/config-original"
[[ -f "$root/normal.key" && -f "$root/message.key" ]]

# ChatGPT auth remains in its selected shared store, including refresh-capable
# credentials loaded on a second run after the source auth.json is removed.
if [[ "$scope" == mock ]]; then
mkdir -p "$HOME/.codex"
printf '%s\n' '{"auth_mode":"chatgpt","tokens":{"id_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","access_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","refresh_token":"fixture","account_id":"fixture"}}' >"$HOME/.codex/auth.json"
export CAI_TEST_API_BASE_URL="$endpoint"
cp "$root/message.key" "$root/message-key-before"
for source in codex stored; do
  plan '{"plan":["AUTH TEMP DONE"]}'
  run --temp --yes -p chatgpt --auth-store messages -ni 'shared auth'; ok
  [[ $(stdout_text) == 'AUTH TEMP DONE' ]]
  cmp "$root/message.key" "$root/message-key-before"
  if [[ "$source" == codex ]]; then rm "$HOME/.codex/auth.json"; fi
done
unset CAI_TEST_API_BASE_URL
fi

# Runtime reopen, /new, and enabling/disabling messaging retain store handles.
# Count SDK opens, rather than infer reuse from the eventual output.
base=("$controls" --config "$config" --no-terminal -C "$work"
      --sessions-store sessions --messaging-store sessions)
export CAI_TEST_APP_CONTROL=stores
for temporary in false true; do
  flags=()
  if [[ "$temporary" == true ]]; then
    flags=(--temp --force)
    export CAI_TEST_APP_STORE_OPENS=2
  else
    export CAI_TEST_APP_STORE_OPENS=1
  fi
  terminal_start "${flags[@]}"
  send '/queue on\r'; expect 'Message queue:'
  send '/queue off\r'; expect 'Messaging is disabled'
  send '/new\r'; expect 'New session'
  send '/queue on\r'; expect 'Message queue:'
  terminal_close
done
unset CAI_TEST_APP_STORE_OPENS

# Host STOP while waiting for the deletion answer must finish without input.
base=("$controls" --config "$config" --no-terminal -C "$work")
mkfifo "$root/stop-signal"
export CAI_TEST_APP_CONTROL=temp_stop CAI_TEST_APP_TEMP_SIGNAL="$root/stop-signal"
terminal_start --temp
send '/quit\r'
wait_for has "$term/text" '[y/N]'
printf x >"$root/stop-signal"
wait_for test -s "$term/status"
wait "$terminal_pid"; terminal_pid=
[[ $(cat "$term/status") == 0 ]]
contains "$term/text" 'Kept temporary workspace:'

# The default global config is used too; neither path is rewritten by --temp.
unset CAI_TEST_APP_CONTROL CAI_TEST_APP_TEMP_SIGNAL
cp "$config" "$root/config/cai/config.ini"
base=("$cli" --no-terminal -C "$work")
plan '{"plan":["GLOBAL CONFIG DONE"]}'
run --temp --yes -ni 'global configuration'; ok
[[ $(stdout_text) == 'GLOBAL CONFIG DONE' ]]
cmp "$config" "$root/config-original"
cmp "$root/config/cai/config.ini" "$root/config-original"
cat "$config" >"$root/temporary.ini"
printf '\n[ui]\ntemporary = true\n' >>"$root/temporary.ini"
base=("$cli" --config "$root/temporary.ini" --no-terminal -C "$work")
run --no-temp --list; ok
plan '{"plan":["CONFIG TEMP DONE"]}'
run --yes -ni 'temporary mode from ini'; ok
[[ $(stdout_text) == 'CONFIG TEMP DONE' ]]
