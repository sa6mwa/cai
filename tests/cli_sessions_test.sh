#!/usr/bin/env bash
set -euo pipefail

cli=$1
build_dir=$2
fixture=$(mktemp -d "$build_dir/cli-sessions.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/work-a" "$fixture/work-b" "$fixture/state"
export XDG_STATE_HOME="$fixture/state" OPENAI_API_KEY=fixture

first=$(printf '/status\n/resume\n/quit\n' | "$cli" -p openai -N -C "$fixture/work-a")
[[ "$first" == *"Status"* && "$first" == *"Model"* && "$first" == *"Reasoning effort"* ]]
first_id=$(printf '%s\n' "$first" | sed -n 's/^1  \([^ ]*\).*/\1/p')
[[ -n "$first_id" ]]
[[ -f "$fixture/state/cai/pouch.key" && -d "$fixture/state/cai/pouch" ]]
[[ ! -d "$fixture/state/cai/sessions" && ! -f "$fixture/state/cai/auth.json" ]]

"$cli" --export "$first_id" -C "$fixture/work-a" >/dev/null
journal="$fixture/state/cai/exports/$first_id/$first_id.jsonl"
cat >> "$journal" <<'JSON'
{"record_type":"event","sequence":2,"type":"assistant_text_delta","data":"Saved assistant answer"}
{"record_type":"event","sequence":3,"type":"assistant_text_end","data":null}
JSON
imported_id=$("$cli" --import "$journal" -C "$fixture/work-a")
[[ -n "$imported_id" && "$imported_id" != "$first_id" ]]
resumed=$(printf '/resume\n/quit\n' | "$cli" -p openai -C "$fixture/work-a")
[[ "$resumed" == *"1  $imported_id"* && "$resumed" == *"Saved assistant answer"* ]]

second=$(printf '/resume\n/quit\n' | "$cli" -p openai --new -C "$fixture/work-a")
second_id=$(printf '%s\n' "$second" | sed -n 's/^1  \([^ ]*\).*/\1/p')
[[ -n "$second_id" && "$second_id" != "$imported_id" ]]
[[ "$second" == *"2  $imported_id"* ]]
selected=$(printf '/resume 2\n/quit\n' | "$cli" -p openai -C "$fixture/work-a")
[[ "$selected" == *"Saved assistant answer"* ]]
exact=$(printf '/quit\n' | "$cli" -p openai --resume "$imported_id" -C "$fixture/work-a")
[[ "$exact" == *"Saved assistant answer"* ]]
isolated=$(printf '/resume\n/quit\n' | "$cli" -p openai --new -C "$fixture/work-b")
[[ "$isolated" != *"$first_id"* && "$isolated" != *"$second_id"* && "$isolated" != *"$imported_id"* ]]
all=$("$cli" -l -C "$fixture/work-b")
[[ "$all" == *"$first_id"* && "$all" == *"$second_id"* && "$all" == *"$imported_id"* ]]
numbered=$("$cli" --resume -C "$fixture/work-a")
[[ "$numbered" == *"1  $second_id"* && "$numbered" == *"2  $imported_id"* ]]
slash_export=$(printf '/export\n/quit\n' | "$cli" -p openai -C "$fixture/work-b")
[[ "$slash_export" == *".jsonl"* && "$slash_export" == *".md"* && "$slash_export" != *"error"* ]]
if printf '/quit\n' | "$cli" -p openai --resume "$first_id" -C "$fixture/work-b" >"$fixture/out" 2>"$fixture/err"; then
  echo 'cross-directory resume was accepted' >&2
  exit 1
fi
grep -q 'another directory' "$fixture/err"
