#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mkdir "$root/lockd"
lockd="$root/lockd"
start_lockd() {
  rm -f "$lockd/port"
  "${CAI_TEST_PYTHON:-python3}" "$support/fixtures/lockd_peer.py" "$lockd" "$@" >"$lockd/stdout" 2>"$lockd/stderr" & peer_pid=$!
  wait_for test -s "$lockd/port"
  scheme=http; [[ $# == 0 ]] || scheme=https
  endpoint="$scheme://127.0.0.1:$(cat "$lockd/port")"
  base=("$cli" -C "$work" --lockd "$endpoint")
}
start_lockd
for option in --list --resume; do
  run "$option"; bad
  contains "$root/stderr" 'Session history requires a Pouch endpoint'
done
[[ ! -e "$lockd/requests.jsonl" ]]
[[ ! -e "$root/state/cai/pouch.key" && ! -e "$root/state/cai/pouch" ]]
# Standalone messaging still reaches remote lockd and retains transport errors.
run -q cai.fixture -M 'remote message'; ok
json "$root/stdout" '.status == "queued" and .recipient == "cai.fixture"'
tail -1 "$lockd/requests.jsonl" >"$root/request"
json "$root/request" '.path == "/v1/queue/enqueue" and .body.meta.namespace == "cai.messages" and .body.meta.queue == "cai.fixture" and .body.payload.text == "remote message"'
printf failure >"$lockd/mode"
run -q cai.fixture -M 'failed remote message'; bad
contains "$root/stderr" 503
contains "$root/stderr" fixture-lockd
kill "$peer_pid"; wait "$peer_pid" 2>/dev/null || true; peer_pid=
printf success >"$lockd/mode"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -subj '/CN=CAI test CA' -keyout "$lockd/ca.key" -out "$lockd/ca.crt" >"$lockd/openssl.log" 2>&1
for name in server client; do
  openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=$name" -keyout "$lockd/$name.key" -out "$lockd/$name.csr" >>"$lockd/openssl.log" 2>&1
  extension='extendedKeyUsage=clientAuth'; [[ "$name" != server ]] || extension='subjectAltName=IP:127.0.0.1'
  printf '%s\n' "$extension" >"$lockd/$name.ext"
  openssl x509 -req -in "$lockd/$name.csr" -CA "$lockd/ca.crt" -CAkey "$lockd/ca.key" -CAcreateserial -days 1 -extfile "$lockd/$name.ext" -out "$lockd/$name.crt" >>"$lockd/openssl.log" 2>&1
done
cat "$lockd/client.crt" "$lockd/client.key" "$lockd/ca.crt" >"$lockd/client.pem"; chmod 600 "$lockd/client.pem"
start_lockd tls
run --lockd-client-pem "$lockd/client.pem" -q cai.fixture -M 'secure message'; ok
json "$root/stdout" '.status == "queued" and .recipient == "cai.fixture"'
tail -1 "$lockd/requests.jsonl" >"$root/request"
json "$root/request" '.path == "/v1/queue/enqueue" and .body.payload.text == "secure message"'
