#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mkdir -p "$root/.codex"
printf '%s\n' '{"auth_mode":"chatgpt","tokens":{"id_token":"fixture","access_token":"e30.eyJleHAiOiA0MTAyNDQ0ODAwfQ.fixture","refresh_token":"fixture","account_id":"fixture"}}' >"$root/.codex/auth.json"
mkfifo "$root/began" "$root/release"
exec 8<>"$root/began" 9<>"$root/release"
export CAI_TEST_QUOTA_STARTED=8 CAI_TEST_QUOTA_RELEASE=9
base=("$cli" -C "$work")
terminal_start -N
read -r -n1 -t5 began <&8; [[ "$began" == Q ]]
# The API is blocked, but prompt and help are usable.
send '/help\r'; expect /resume
printf 1 >&9; expect 'w 88%'; expect 'crd 20'
if read -r -n1 -t .1 began <&8; then fail 'Idle quota polling'; fi
send '/status\r'; read -r -n1 -t5 began <&8; [[ "$began" == Q ]]
printf E >&9; expect Status
if read -r -n1 -t .1 began <&8; then fail 'Failed fetch ignored debounce'; fi
send '/status\r'; read -r -n1 -t5 began <&8; [[ "$began" == Q ]]; printf 1 >&9
terminal_close
export CAI_TEST_QUOTA_TIMEOUT_MS=23456
export CAI_TEST_QUOTA_CA_BUNDLE="$root/trust.pem" CAI_TEST_QUOTA_CA_PATH="$root/certs"
export CAI_TEST_QUOTA_HTTP2_DISABLED=1
for insecure in false true; do
  if [[ "$insecure" == true ]]; then export CAI_TEST_QUOTA_INSECURE=1; else export CAI_TEST_QUOTA_INSECURE=0; fi
  printf '[client]\ntimeout_ms=23456\nca_bundle_path=trust.pem\nca_path=certs\ninsecure_skip_verify=%s\nhttp_2_disabled=true\n' "$insecure" >"$root/timeout.ini"
  terminal_start -N --config "$root/timeout.ini"
  read -r -n1 -t5 began <&8; [[ "$began" == Q ]]
  printf 1 >&9; expect 'w 88%'
  terminal_close
done
unset CAI_TEST_QUOTA_TIMEOUT_MS
unset CAI_TEST_QUOTA_CA_BUNDLE CAI_TEST_QUOTA_CA_PATH
unset CAI_TEST_QUOTA_INSECURE CAI_TEST_QUOTA_HTTP2_DISABLED
export BERGET_API_KEY=fixture
base=("$cli" -p berget -C "$work")
terminal_start -N
if read -r -n1 -t .1 began <&8; then fail 'Berget startup fetched quota'; fi
terminal_close
exec 8>&- 9>&-
