#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
lua_runner=${1:-}; lua_modules=${2:-}
export LOG_LEVEL=warn
peer_start
# Large, highly compressible output makes compression observable, while each
# provider call is a local fixture response with no delay or real inference.
plan "$(jq -nc '{plan:[("FIRST-CONTEXT-λ " + ("abcdefgh" * 24576))]}')"
run -Nni 'FIRST JOURNAL PROMPT'; ok
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
grep -qx 'compression=zlib' "$root/state/cai/pouch/manifest"
for n in 1 2 3; do
  plan "$(jq -nc --arg n "$n" '{plan:[("REPLY " + $n)]}')"
  run -ni "PROMPT $n"; ok
  [[ $(stdout_text) == "REPLY $n" ]]
  tail -1 "$root/peer/requests.jsonl" | jq -c . >"$root/request"
  contains "$root/request" 'FIRST JOURNAL PROMPT'
  contains "$root/request" 'FIRST-CONTEXT-λ'
done
run --export "$id"; ok
exported=$(head -1 "$root/stdout")
cp "$exported" "$root/native.jsonl"
if [[ -n "$lua_runner" ]]; then
  LUA_CPATH="$lua_modules/?.so;;" "$lua_runner" "$support/lua/journal_test.lua" "$root/native.jsonl" "$work"
fi
jq -se 'any(.[]; .record_type=="commit") and all(.[];
  .record_type=="event" or (.record_type=="commit" and
    .state.metadata.version==1 and (.state|has("history")|not)))' "$exported" >/dev/null
[[ $(find "$root/state/cai/pouch/namespaces/cai.sessions/collections/names" -type f | wc -l) == 1 ]]
# A real encrypted second root, with compression explicitly disabled, imports
# and resumes the exact same conversation. Only the storage mode differs.
run --disable-compression --state-dir "$root/uncompressed" --import "$exported"; ok
imported=$(stdout_text)
grep -qx 'compression=none' "$root/uncompressed/pouch/manifest"
plan '{"plan":["IMPORTED CONTINUATION"]}'
run --disable-compression --state-dir "$root/uncompressed" --resume "$imported" -ni 'Continue after import'; ok
tail -1 "$root/peer/requests.jsonl" | jq -c . >"$root/request"
contains "$root/request" 'FIRST-CONTEXT-λ'; contains "$root/request" 'PROMPT 3'; contains "$root/request" 'REPLY 3'
compressed=$(find "$root/state/cai/pouch/namespaces/cai.sessions/collections/data" -name 'seg-*.log' -printf '%s\n' | awk '{n+=$1} END{print n}')
plain=$(find "$root/uncompressed/pouch/namespaces/cai.sessions/collections/data" -name 'seg-*.log' -printf '%s\n' | awk '{n+=$1} END{print n}')
(( compressed * 2 < plain )) || fail "Compression not effective: $compressed vs $plain bytes"
run --disable-compression --list; bad; contains "$root/stderr" compression
# Environment and INI surfaces select the same setting; an explicit CLI flag
# overrides configuration. Mismatching an existing root fails rather than
# silently changing its storage mode.
CAI_DISABLE_COMPRESSION=true run --state-dir "$root/env-state" --list; ok
grep -qx 'compression=none' "$root/env-state/pouch/manifest"
printf '[storage]\ndisable_compression=true\n' >"$root/storage.ini"
run --config "$root/storage.ini" --state-dir "$root/ini-state" --list; ok
grep -qx 'compression=none' "$root/ini-state/pouch/manifest"
run --config "$root/storage.ini" --no-disable-compression --state-dir "$root/cli-state" --list; ok
grep -qx 'compression=zlib' "$root/cli-state/pouch/manifest"
run --dump-config; ok; contains "$root/stdout" '# disable_compression = false'
# Bad imports cannot become resumable/listable conversations.
run --list; ok; cp "$root/stdout" "$root/list-before"
for variant in truncated duplicate revision object_data; do
  if [[ "$variant" == truncated ]]; then
    head -c -9 "$root/native.jsonl" >"$root/bad.jsonl"
  elif [[ "$variant" == duplicate ]]; then
    cat "$root/native.jsonl" "$root/native.jsonl" >"$root/bad.jsonl"
  elif [[ "$variant" == revision ]]; then
    jq -c 'if .record_type=="commit" then .state.revision=2 else . end' "$root/native.jsonl" >"$root/bad.jsonl"
  else
    jq -c 'if .sequence==1 then .data={unexpected:true} else . end' "$root/native.jsonl" >"$root/bad.jsonl"
  fi
  run --import "$root/bad.jsonl"; bad
  run --list; ok; cmp "$root/stdout" "$root/list-before"
done
