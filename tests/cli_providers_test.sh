#!/usr/bin/env bash
set -euo pipefail

cli=$1
build_dir=$2
fixture=$(mktemp -d "$build_dir/cli-providers.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/home/.codex" "$fixture/state/cai" "$fixture/work"
export HOME="$fixture/home" XDG_STATE_HOME="$fixture/state"
cat > "$fixture/state/cai/auth.json" <<'JSON'
{"auth_mode":"chatgpt","tokens":{"id_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","access_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","refresh_token":"fixture","account_id":"fixture"},"last_refresh":"2026-09-25T00:00:00Z"}
JSON
export HTTPS_PROXY=http://127.0.0.1:1 HTTP_PROXY=http://127.0.0.1:1 NO_PROXY=
status=$(printf '/status\n/quit\n' | "$cli" -C "$fixture/work")
[[ "$status" == *"Provider"* && "$status" == *"chatgpt"* ]]
[[ "$status" == *"Model"* && "$status" == *"gpt-6-luna"* ]]

printf 'invalid\n' > "$fixture/home/.codex/auth.json"
if printf '/quit\n' | "$cli" -C "$fixture/work" >"$fixture/out" 2>"$fixture/err"; then
  echo 'Codex auth was not preferred' >&2
  exit 1
fi
grep -q 'open ChatGPT auth' "$fixture/err"
rm "$fixture/home/.codex/auth.json"

openai=$(printf '/status\n/quit\n' | OPENAI_API_KEY=fixture "$cli" -p openai -C "$fixture/work")
[[ "$openai" == *"Provider"* && "$openai" == *"openai"* ]]
[[ "$openai" != *"Weekly limit"* && "$openai" != *"Credits left"* ]]
router=$(printf '/status\n/quit\n' | OPENROUTER_API_KEY=fixture "$cli" -p openrouter -C "$fixture/work")
[[ "$router" == *"Provider"* && "$router" == *"openrouter"* ]]
[[ "$router" == *"Model"* && "$router" == *"openai/gpt-5.6-luna"* ]]
custom=$(printf '/status\n/quit\n' | LOCAL_KEY=fixture "$cli" -p custom \
  --endpoint https://example.test/v1 --api-key-env LOCAL_KEY -m local-model -C "$fixture/work")
[[ "$custom" == *"Provider"* && "$custom" == *"custom"* ]]
[[ "$custom" == *"Model"* && "$custom" == *"local-model"* ]]
default_key=$(printf '/status\n/quit\n' | CAI_API_KEY=fixture "$cli" -p custom \
  --endpoint https://example.test/v1 -m local-model -C "$fixture/work")
[[ "$default_key" == *"custom"* ]]
if printf '/quit\n' | env -u OPENAI_API_KEY "$cli" -p openai -C "$fixture/work" \
  >"$fixture/out" 2>"$fixture/err"; then
  echo 'missing OpenAI key was accepted' >&2
  exit 1
fi
grep -q 'OPENAI_API_KEY' "$fixture/err"

rm "$fixture/state/cai/auth.json"
if printf '/quit\n' | "$cli" -C "$fixture/work" >"$fixture/out" 2>"$fixture/err"; then
  echo 'missing auth was accepted' >&2
  exit 1
fi
grep -q 'run cai --login (-l)' "$fixture/err"
"$cli" --help | grep -q 'cai and libcai Copyright (C) 2026 C89 Systems AB https://c89.systems'
