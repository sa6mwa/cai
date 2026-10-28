#!/usr/bin/env bash
# Damage only a disposable store's derived index, never authoritative records.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":["DURABLE CONVERSATION ANSWER"]}'
run -Nni 'durable conversation prompt'; ok
run --list; ok
cp "$root/stdout" "$root/list"
id=$(awk 'NR == 1 {print $1}' "$root/list")
[[ -n "$id" ]]
manifest="$root/state/cai/pouch/namespaces/cai.sessions/index/query.manifest"
key="$root/state/cai/pouch.key"
[[ -s "$manifest" ]]
cp "$key" "$root/original-key"
for mode in missing empty malformed; do
  case "$mode" in
    missing) rm "$manifest" ;;
    empty) : >"$manifest" ;;
    malformed) printf 'interrupted derived index\n' >"$manifest" ;;
  esac
  run --list; ok
  cmp "$root/stdout" "$root/list"
  cmp "$key" "$root/original-key"
  [[ -s "$manifest" ]]
  # Recovery must install a readable index; the warning must not recur.
  run --list; ok
  cmp "$root/stdout" "$root/list"
  absent "$root/stderr" cache.load.index.error
done
run --export "$id"; ok
contains "$root/data/cai/exports/$id/$id.md" 'DURABLE CONVERSATION ANSWER'
contains "$root/data/cai/exports/$id/$id.jsonl" 'durable conversation prompt'
plan '{"plan":["RESUMED AFTER INDEX RECOVERY"]}'
run -ni 'continue after recovering the index'; ok
contains "$root/stdout" 'RESUMED AFTER INDEX RECOVERY'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'durable conversation prompt'
contains "$root/request" 'DURABLE CONVERSATION ANSWER'
# Auto-resume followed immediately by /quit must complete recovery without
# a user prompt, /resume listing, or a commit-producing model turn.
count=$(requests)
# An unusable derived-index path must report the repair failure without
# preventing direct auto-resume of the intact conversation.
rm "$manifest"
mkdir "$manifest"
terminal_start
expect 'Session search index recovery failed'
expect 'RESUMED AFTER INDEX RECOVERY'
terminal_close
rmdir "$manifest"
printf 'interrupted derived index\n' >"$manifest"
for mode in missing empty malformed; do
  case "$mode" in
    missing) rm "$manifest" ;;
    empty) : >"$manifest" ;;
    malformed) printf 'interrupted derived index\n' >"$manifest" ;;
  esac
  terminal_start
  expect 'RESUMED AFTER INDEX RECOVERY'
  if [[ "$mode" != missing ]]; then
    expect 'Search index cache unavailable'
    expect 'recovery from stored records'
  fi
  terminal_close
  for restart in 1 2; do
    terminal_start
    expect 'RESUMED AFTER INDEX RECOVERY'
    absent "$term/text" 'Search index cache unavailable'
    terminal_close
  done
done
terminal_start
send '/resume\r'; expect "$id"
terminal_close
[[ $(requests) == "$count" ]]
run --list; ok
absent "$root/stderr" cache.load.index.error
cmp "$key" "$root/original-key"
# Auto-resume reads the workspace lookup directly too. Repair its derived
# index during open rather than waiting for a later indexed query or prompt.
manifest="$root/state/cai/pouch/namespaces/cai.scopes/index/query.manifest"
[[ -s "$manifest" ]]
printf 'interrupted workspace index\n' >"$manifest"
terminal_start
expect 'RESUMED AFTER INDEX RECOVERY'
terminal_close
terminal_start
expect 'RESUMED AFTER INDEX RECOVERY'
absent "$term/text" 'Search index cache unavailable'
terminal_close
[[ $(requests) == "$count" ]]
cmp "$key" "$root/original-key"
