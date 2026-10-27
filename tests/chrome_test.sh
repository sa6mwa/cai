#!/usr/bin/env bash
set -euo pipefail
mode=$1; shift
source "$(dirname "$0")/cli_test_common.sh"
export LOG_LEVEL=warn LOG_MODE=json XDG_RUNTIME_DIR="$root/run"
unset CHROME_CONFIG_HOME CODEX_HOME
mkdir -m700 "$XDG_RUNTIME_DIR"
chrome="$root/chrome"; mkdir "$chrome"
scenario() {
  rm -f "$chrome/ready" "$chrome/mock.sock" "$chrome/calls.jsonl"
  "${CAI_TEST_PYTHON:-python3}" "$support/fixtures/chrome_peer.py" socket "$chrome" "$1" >"$chrome/stdout" 2>"$chrome/stderr" & chrome_pid=$!
  wait_for test -s "$chrome/ready"
}
chrome_cleanup() {
  rc=$?
  for owned in "${chrome_pid:-}" "${extension_pid:-}" "${host_pid:-}"; do if [[ -n "$owned" ]]; then kill "$owned" 2>/dev/null || true; wait "$owned" 2>/dev/null || true; fi; done
  cleanup "$rc"
}
trap chrome_cleanup EXIT
client_run() {
  rc=0; "$cli" "$chrome/mock.sock" "$@" >"$root/stdout" 2>"$root/stderr" || rc=$?
  wait "$chrome_pid"; chrome_pid=
  absent "$root/stderr" AddressSanitizer; absent "$root/stderr" 'runtime error:'
}
if [[ "$mode" == client ]]; then
  for fault in fragment null failure wrong-id missing-result limit oversized malformed truncated timeout cancel identity identity-missing-result; do
    scenario "$fault"
    operation=request; [[ "$fault" != limit ]] || operation=limit
    [[ "$fault" != cancel ]] || operation=cancel
    [[ "$fault" != identity ]] || operation=open
    client_run "$operation"
    case "$fault" in fragment) ok; contains "$root/stdout" Fixture ;; null) ok; contains "$root/stdout" null ;; *) bad; contains "$root/stderr" status= ;; esac
  done
  for method in attach detach markTab finalizeTabs nameSession turnEnded executeCdp; do
    scenario void; client_run method "$method"; ok
    [[ $(stdout_text) == null ]]
  done
  for fault in void void-wrong-id void-no-id void-error void-malformed-error void-both void-version; do
    scenario "$fault"; client_run tool '{"action":"claim","tab_id":7}'
    if [[ "$fault" == void ]]; then ok; else bad; contains "$root/stderr" status=; fi
    if [[ "$fault" == void-error ]]; then contains "$root/stderr" 'Debugger unattached'; fi
  done
  for args in '{"action":"open","url":"https://example.test/"}' '{"action":"claim","tab_id":7}' '{"action":"tabs"}' '{"action":"snapshot","tab_id":7}' '{"action":"click","tab_id":7,"x":12,"y":20}' '{"action":"key","tab_id":7,"text":"Enter"}' '{"action":"scroll","tab_id":7,"x":1,"y":2,"delta_y":-10}' '{"action":"type","tab_id":7,"text":"quote\"\nline"}' '{"action":"evaluate","tab_id":7,"text":"document.title"}' '{"action":"keep","tab_id":7}' '{"action":"close","tab_id":7}' '{"action":"navigate","tab_id":7,"url":"https://example.test/?a=b"}' '{"action":"cdp","tab_id":7,"method":"Runtime.evaluate","params":{"expression":"1","returnByValue":true}}' '{"action":"screenshot","tab_id":7}'; do
    scenario success; client_run tool "$args"; ok
    jq -se '.[0].method == "getInfo" and .[-1].method == "turnEnded" and all(.[1:][]; .params.session_id == "fixture" and .params.turn_id == "1")' "$chrome/calls.jsonl" >/dev/null
  done
  for args in '{"action":"click","tab_id":1}' '{"action":"open","url":"file:///etc/passwd"}' '{"action":"navigate","tab_id":1,"url":"file:///etc/passwd"}' '{"action":"nonsense"}' '{"action":null}' '{}' '[]' 'null' '{"action":"key","tab_id":7,"text":null}' '{"action":"cdp","tab_id":7,"method":"Runtime.evaluate","params":null}'; do
    scenario success; client_run tool "$args"; bad
  done
  scenario missing-result; client_run tool '{"action":"open"}'; bad
  for fault in image-invalid image-null void-cdp-missing-image; do scenario "$fault"; client_run tool '{"action":"screenshot","tab_id":7}'; bad; done
  exit
fi
if [[ "$mode" == cli ]]; then
  base=("$cli")
  run --chrome-remove; ok
  run --chrome-setup; ok
  manifest="$root/config/google-chrome/NativeMessagingHosts/com.openai.codexextension.json"
  json "$manifest" ".path == \"$cli\" and .name == \"com.openai.codexextension\" and .type == \"stdio\" and .allowed_origins == [\"chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/\"]"
  [[ $(stat -c %a "$manifest") == 600 && ! -e "$root/state" ]]
  cp "$manifest" "$root/good-manifest"
  run --chrome-setup; ok; cmp "$manifest" "$root/good-manifest"
  run --chrome-remove; ok; [[ ! -e "$manifest" ]]
  for payload in 'not-json' '{"name":null}' '{"path":"/opt/openai/extension-host"}'; do
    printf '%s\n' "$payload" >"$manifest"; chmod 600 "$manifest"
    cp "$manifest" "$root/snapshot"
    for option in --chrome-setup --chrome-remove; do run "$option"; bad; cmp "$manifest" "$root/snapshot"; done
    rm "$manifest"
  done
  printf 'do not touch' >"$root/foreign.json"; ln -s "$root/foreign.json" "$manifest"
  run --chrome-setup; bad; run --chrome-remove; bad
  [[ -L "$manifest" && $(cat "$root/foreign.json") == 'do not touch' ]]; rm "$manifest"
  mkfifo "$manifest"; run --chrome-setup; bad; rm "$manifest"
  chmod 770 "$(dirname "$manifest")"; run --chrome-setup; bad; contains "$root/stderr" 'writable by others'; chmod 700 "$(dirname "$manifest")"
  run --chrome-setup --chrome-manifest "$root/chromium/NativeMessagingHosts/com.openai.codexextension.json"; ok
  run --chrome-remove --chrome-manifest "$root/chromium/NativeMessagingHosts/com.openai.codexextension.json"; ok
fi
if [[ "$mode" == host || "$mode" == cli ]]; then
  if [[ "$mode" == cli ]]; then peer_start; fi
  # Bash owns host execution. The extension fixture only responds on stdio.
  mkfifo "$chrome/host-input" "$chrome/extension-input"
  exec 8<>"$chrome/host-input" 9<>"$chrome/extension-input"
  LOG_LEVEL=info "$cli" chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/ <"$chrome/host-input" >"$chrome/extension-input" 2>"$chrome/host-stderr" 8>&- 9>&- & host_pid=$!
  "${CAI_TEST_PYTHON:-python3}" "$support/fixtures/chrome_peer.py" extension "$chrome" "${3:-void}" <"$chrome/extension-input" >"$chrome/host-input" 8>&- 9>&- & extension_pid=$!
  if [[ "$mode" == host ]]; then
    # A compiled host-only fixture follows the same entry point.
    wait_for test -d "$root/run/cai/chrome"
    client=${2:?compiled Chrome client fixture}
    wait_for test -S "$root/run/cai/chrome/$host_pid.sock"
    "$client" "$root/run/cai/chrome/$host_pid.sock" request >"$root/stdout" 2>"$root/stderr"
    contains "$root/stdout" Fixture
    "$client" "$root/run/cai/chrome/$host_pid.sock" tool '{"action":"claim","tab_id":7}' >"$root/stdout" 2>"$root/stderr"
    jq -se 'any(.[]; .method == "attach") and .[-1].method == "turnEnded"' "$chrome/calls.jsonl" >/dev/null
    for cleanup_mode in multi explicit; do
      if [[ "${3:-void}" != manual-detach || "$cleanup_mode" == multi ]]; then
        "$client" "$root/run/cai/chrome/$host_pid.sock" "cleanup-$cleanup_mode" >"$root/stdout" 2>"$root/stderr"
        jq -se --arg session "$cleanup_mode" '[.[] | select(.params.session_id == $session and .method == "detach") | .params.tabId] | sort == [7,8]' "$chrome/calls.jsonl" >/dev/null
      fi
    done
    "$client" "$root/run/cai/chrome/$host_pid.sock" abandon >"$root/stdout" 2>"$root/stderr"
    # This reverse ping follows the cleanup reply on the same extension pipe;
    # its response proves the host consumed turnEnded before the next client.
    wait_for has "$chrome/host-replies.jsonl" cleanup-ping
    "$client" "$root/run/cai/chrome/$host_pid.sock" request >"$root/stdout" 2>"$root/stderr"
    contains "$root/stdout" Fixture
    jq -se 'any(.[]; .method == "turnEnded" and .params.session_id == "abandoned")' "$chrome/calls.jsonl" >/dev/null
    jq -se '.[0].method == "getInfo" and any(.[]; .method == "getUserTabs")' "$chrome/calls.jsonl" >/dev/null
  else
    # Readiness is independent of the directory under test. SDK discovery
    # must find the application's host without an explicit path override.
    wait_for has "$chrome/host-stderr" 'CAI Chrome native host ready'
    client=${2:?compiled Chrome client fixture}
    "$client" "$root" default >"$root/stdout" 2>"$root/stderr"
    contains "$root/stdout" Fixture
    plan '{"plan":[{"tool":"chrome","arguments":{"action":"open","url":"https://example.test/"}},{"tool":"chrome","arguments":{"action":"screenshot","tab_id":7}},{"tool":"chrome","arguments":{"action":"keep","tab_id":7}},"Chrome final response"]}'
    run -Nni 'Inspect the browser' --chrome; ok
    [[ $(stdout_text) == 'Chrome final response' ]]
    jq -se '[.[].method] == ["getInfo","getUserTabs","getInfo","createTab","attach","executeCdp","executeCdp","markTab","detach","turnEnded"]' "$chrome/calls.jsonl" >/dev/null
    sed -n '3p' "$root/peer/requests.jsonl" >"$root/image-request"; contains "$root/image-request" 'data:image/png;base64,'
    sed -n '4p' "$root/peer/requests.jsonl" >"$root/durable-request"; absent "$root/durable-request" 'data:image/png;base64,'
  fi
  jq -se 'any(.[]; .id == "extension-ping" and .result == "pong")' "$chrome/host-replies.jsonl" >/dev/null
  jq -se 'length > 0 and all(.[]; .tabs == [])' "$chrome/active-tabs.jsonl" >/dev/null
  if [[ "${3:-void}" == manual-detach ]]; then
    contains "$chrome/cleanup-errors.jsonl" 'Debugger is not attached'
  fi
  kill "$extension_pid"
  wait "$extension_pid" 2>/dev/null || true
  extension_pid=
  exec 8>&- 9>&-
  wait "$host_pid"
  [[ ! -e "$root/run/cai/chrome/$host_pid.sock" ]]
  host_pid=
fi
if [[ "$mode" == lua ]]; then
  # Lua owns API assertions in the existing compiled facade scenario.
  scenario void
  peer_start
  plan '{"plan":[{"tool":"chrome","arguments":{"action":"open","url":"https://example.test/"}},{"tool":"chrome","arguments":{"action":"screenshot","tab_id":7}},{"tool":"chrome","arguments":{"action":"keep","tab_id":7}},"Chrome final response"]}'
  "$cli" "$support/lua/e2e_chrome.lua" "$chrome/mock.sock" "$endpoint" "$root" close
  wait "$chrome_pid"; chrome_pid=
  jq -se '.[-1].method == "turnEnded"' "$chrome/calls.jsonl" >/dev/null
fi
