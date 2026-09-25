#!/usr/bin/env bash
set -euo pipefail

cli=$1
build_dir=$2
fixture=$(mktemp -d "$build_dir/cli-sessions.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/work-a" "$fixture/work-b" "$fixture/state"
cat > "$fixture/auth.json" <<'JSON'
{"auth_mode":"chatgpt","tokens":{"id_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","access_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","refresh_token":"fixture","account_id":"fixture"},"last_refresh":"2026-09-25T00:00:00Z"}
JSON
export XDG_STATE_HOME="$fixture/state"

first=$(printf '/resume\n/quit\n' | "$cli" -n -C "$fixture/work-a" \
  --auth-json "$fixture/auth.json")
first_id=$(printf '%s\n' "$first" | sed -n 's/^  1  \([^ ]*\)  (current)$/\1/p')
[[ -n "$first_id" ]]
journals=("$fixture/state/cai/sessions"/*/"$first_id.jsonl")
[[ ${#journals[@]} -eq 1 && -f "${journals[0]}" ]]
cat >> "${journals[0]}" <<'JSON'
{"record_type":"event","sequence":2,"type":"assistant_text_delta","data":"Saved assistant answer"}
{"record_type":"event","sequence":3,"type":"assistant_text_end","data":null}
JSON

resumed=$(printf '/resume\n/quit\n' | "$cli" -C "$fixture/work-a" \
  --auth-json "$fixture/auth.json")
[[ "$resumed" == *"  1  $first_id  (current)"* ]]
[[ "$resumed" == *"Saved assistant answer"* ]]

second=$(printf '/resume\n/quit\n' | "$cli" --new -C "$fixture/work-a" \
  --auth-json "$fixture/auth.json")
second_id=$(printf '%s\n' "$second" | sed -n 's/^  1  \([^ ]*\)  (current)$/\1/p')
[[ -n "$second_id" && "$second_id" != "$first_id" ]]
[[ "$second" == *"  2  $first_id"* ]]

selected=$(printf '/resume 2\n/resume\n/quit\n' | "$cli" \
  -C "$fixture/work-a" --auth-json "$fixture/auth.json")
[[ "$selected" == *"  2  $first_id  (current)"* ]]
[[ "$selected" == *"Saved assistant answer"* ]]

exact=$(printf '/resume\n/quit\n' | "$cli" --resume "$first_id" \
  -C "$fixture/work-a" --auth-json "$fixture/auth.json")
[[ "$exact" == *"  2  $first_id  (current)"* ]]

isolated=$(printf '/resume\n/quit\n' | "$cli" --new \
  -C "$fixture/work-b" --auth-json "$fixture/auth.json")
[[ "$isolated" != *"$first_id"* && "$isolated" != *"$second_id"* ]]
