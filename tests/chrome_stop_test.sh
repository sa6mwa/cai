#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
client=$(realpath "$1")
export LOG_LEVEL=warn LOG_MODE=json XDG_RUNTIME_DIR="$root/run"
unset CHROME_CONFIG_HOME CODEX_HOME
mkdir -m700 "$XDG_RUNTIME_DIR"
base=("$cli")
origin=chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/
directory="$root/run/cai/chrome"
hosts=() extensions=() holders=()
stop_cleanup() {
  local result=$?
  trap - EXIT
  for owned in "${holders[@]}" "${hosts[@]}" "${extensions[@]}"; do
    kill "$owned" 2>/dev/null || true
    wait "$owned" 2>/dev/null || true
  done
  cleanup "$result"
}
trap stop_cleanup EXIT
start_host() {
  local binary=$1 folder=$2 scenario=${3:-void}
  mkdir "$folder"
  mkfifo "$folder/to-host" "$folder/to-extension"
  exec 8<>"$folder/to-host" 9<>"$folder/to-extension"
  "$binary" "$origin" <"$folder/to-host" >"$folder/to-extension" 2>"$folder/stderr" 8>&- 9>&- &
  host=$!; hosts+=("$host")
  "${CAI_TEST_PYTHON:-python3}" "$support/fixtures/chrome_peer.py" extension "$folder" "$scenario" \
    <"$folder/to-extension" >"$folder/to-host" 8>&- 9>&- &
  extensions+=("$!")
  wait_for test -S "$directory/$host.sock"
  wait_for test -S "$directory/$host.ctl"
  # Keep no FIFO write descriptors in the test owner: host exit must reach
  # the extension fixture as real EOF.
  exec 8>&- 9>&-
}

run --chrome-setup; ok
start_host "$cli" "$root/idle"
idle=$host
start_host "$cli" "$root/idle-two"
idle_two=$host
"$client" "$directory/stale.sock" stale
"$client" "$directory/stale.ctl" stale
"$client" "$directory/stale-old.sock" stale
cp "$cli" "$root/other-cai"
start_host "$root/other-cai" "$root/foreign"
foreign=$host
"$client" "$directory/$idle.ctl" control shutdown '{"executable":"/different/program"}' >"$root/control"
contains "$root/control" different-executable
"$client" "$directory/$idle.ctl" control unsupported '{}' >"$root/control" 2>"$root/control-error" && fail 'Unsupported control accepted'
run --chrome-remove; ok; contains "$root/stdout" 'Stopped 2'
wait "$idle"; wait "$idle_two"
[[ ! -e "$directory/$idle.sock" && ! -e "$directory/$idle.ctl" ]]
[[ -S "$directory/$foreign.sock" && -S "$directory/$foreign.ctl" ]]
"$client" "$directory/$foreign.sock" request >"$root/control" 2>"$root/control-error"
contains "$root/control" Fixture
"$root/other-cai" --chrome-remove >"$root/foreign-remove" 2>"$root/foreign-error"
wait "$foreign"
run --chrome-remove; ok

# A live browser socket with a missing or stale control endpoint is not a
# stopped host. Report it, leave it running, then prove retry after repair.
for control_state in missing stale; do
  start_host "$cli" "$root/$control_state-control"
  current=$host
  mv "$directory/$current.ctl" "$directory/$current.saved"
  if [[ "$control_state" == stale ]]; then
    "$client" "$directory/$current.ctl" stale
  fi
  run --chrome-remove; bad
  contains "$root/stderr" 'no shutdown endpoint'
  [[ -S "$directory/$current.sock" ]]
  rm -f "$directory/$current.ctl"
  mv "$directory/$current.saved" "$directory/$current.ctl"
  run --chrome-remove; ok; wait "$current"
done

# Configured directories, occupied browser connections and failed cleanup.
mkdir -p "$root/config/cai"
directory="$root/custom-chrome"
printf '[chrome]\nsocket_directory = %s\n' "$directory" >"$root/config/cai/config.ini"
for scenario in void stop-cleanup-fail; do
  run --chrome-setup; ok
  start_host "$cli" "$root/$scenario" "$scenario"
  current=$host
  mkfifo "$root/$scenario/input"
  exec 7<>"$root/$scenario/input"
  "$client" "$directory/$current.sock" hold <"$root/$scenario/input" \
    >"$root/$scenario/holder" 2>"$root/$scenario/holder-error" 7>&- &
  holder=$!; holders+=("$holder")
  wait_for has "$root/$scenario/holder" browser-held
  run --chrome-remove
  if [[ "$scenario" == void ]]; then
    ok; contains "$root/stdout" 'Stopped 1'; wait "$current"
    jq -se '.[-1].tabs == []' "$root/$scenario/active-tabs.jsonl" >/dev/null
  else
    bad; contains "$root/stderr" 'Cleanup fixture rejected turnEnded'
    if wait "$current"; then fail 'Failed cleanup reported host success'; fi
  fi
  [[ ! -e "$directory/$current.sock" && ! -e "$directory/$current.ctl" ]]
  printf x >&7; exec 7>&-
  if wait "$holder"; then fail 'Browser connection survived removal'; fi
  contains "$root/$scenario/holder-error" disconnected
  jq -se '[.[] | select(.method == "detach") | .params.tabId] | sort == [7,8]' "$root/$scenario/calls.jsonl" >/dev/null
done
