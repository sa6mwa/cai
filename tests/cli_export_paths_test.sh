#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
base+=(--export-dir "$root/exports")
plan '{"plan":["EXPORT ANSWER"]}'
run -Nni 'EXPORT PROMPT'; ok
run --list; ok
id=$(awk 'NR==1 {print $1}' "$root/stdout")
before=$(requests)
run --export "$id"; ok
[[ $(head -1 "$root/stdout") == "$root/exports/$id/$id.jsonl" ]]
[[ $(tail -1 "$root/stdout") == "$root/exports/$id/$id.md" ]]
run --export "$id" --export-out ''; ok
contains "$root/stdout" "$root/exports/$id/$id.md"
run --export-out nope.md; bad; contains "$root/stderr" '--export-out requires --export ID'
run --export "$id" --export-out; bad; contains "$root/stderr" 'requires a value'

# The workspace need not be private just because export files are private.
chmod 755 "$work"
check_files() {
  local name=$1 mode=$2 absolute
  absolute=$name; [[ "$name" == /* ]] || absolute="$work/$name"
  if [[ "$mode" != jsonl ]]; then
    local md=$absolute
    [[ "$mode" == md ]] || md+=.md
    [[ -s "$md" && $(stat -c %a "$md") == 600 ]]
    contains "$md" 'EXPORT PROMPT'; contains "$md" 'EXPORT ANSWER'
  fi
  if [[ "$mode" != md ]]; then
    local jsonl=$absolute
    [[ "$mode" == jsonl ]] || jsonl+=.jsonl
    [[ -s "$jsonl" && $(stat -c %a "$jsonl") == 600 ]]
    jq -se '.[0].record_type=="checkpoint" and .[0].state!=null' "$jsonl" >/dev/null
  fi
  if [[ "$mode" == md ]]; then
    [[ ! -e "$absolute.jsonl" && ! -e "${absolute%.md}.jsonl" ]]
  elif [[ "$mode" == jsonl ]]; then
    [[ ! -e "$absolute.md" && ! -e "${absolute%.jsonl}.md" ]]
  else
    [[ ! -e "$absolute" ]]
  fi
  [[ $(stat -c %a "$work") == 755 ]]
}
for spec in 'cli-markdown.md|md' 'cli-json.jsonl|jsonl' 'cli/deep/conversation|both' \
  'cli/other.suffix|both' 'cli/path with spaces.md|md'; do
  name=${spec%|*} mode=${spec##*|}
  run --export-out "$name" --export "$id"; ok
  check_files "$name" "$mode"
done
run --export "$id" --export-out "$root/absolute/conversation.jsonl"; ok
check_files "$root/absolute/conversation.jsonl" jsonl

terminal_start --resume "$id"
for spec in 'myname.md|md' 'conversation.jsonl|jsonl' 'conversation|both' \
  'output/dir/conversation.md|md' 'output/json/conversation.jsonl|jsonl' \
  'output/pair/conversation|both' 'something.else|both' \
  'tui/path with spaces.md|md' "$root/tui-absolute/conversation.md|md" \
  "$id|both"; do
  name=${spec%|*} mode=${spec##*|}
  send "/export $name\r"; expect 'Conversation export'
  # A following local command proves the export and its path rendering finished.
  send '/goal\r'; expect 'No goal.'
  check_files "$name" "$mode"
done
# Overwrite only after successful generation; keep the new file private.
printf 'OLD CONTENT\n' >"$work/overwrite.md"; chmod 644 "$work/overwrite.md"
send '/export overwrite.md\r'; expect 'Conversation export'
send '/goal\r'; expect 'No goal.'
check_files overwrite.md md
absent "$work/overwrite.md" 'OLD CONTENT'
# Invalid destinations must preserve existing siblings and leave no temp files.
mkdir "$work/pair.md"
printf 'UNCHANGED\n' >"$work/pair.jsonl"
send '/export pair\r'; expect 'export destination must be a file'
[[ $(cat "$work/pair.jsonl") == UNCHANGED ]]
printf 'BLOCKED\n' >"$work/blocked"
send '/export blocked/child\r'; expect 'export parent is not a directory'
[[ $(cat "$work/blocked") == BLOCKED && ! -e "$work/blocked.md" ]]
for name in output/ . ..; do
  send "/export $name\r"; expect 'export basename must name a file'
done
long_name=$(printf '%0245d.md' 0)
printf 'KEEP ORIGINAL\n' >"$work/$long_name"
send "/export $long_name\r"; expect 'Conversation export'
send '/goal\r'; expect 'No goal.'
check_files "$long_name" md
absent "$work/$long_name" 'KEEP ORIGINAL'
[[ -z $(find "$work" \( -name '*.tmp.*' -o -name '.export-*' \) -print) ]]
rm "$root/exports/$id/$id.jsonl" "$root/exports/$id/$id.md"
send '/export   \r'; expect 'Conversation export'
send '/goal\r'; expect 'No goal.'
[[ -s "$root/exports/$id/$id.jsonl" && -s "$root/exports/$id/$id.md" ]]
[[ $(requests) == "$before" ]]
terminal_close
# Named JSONL is a genuine resumable export, not just a transcript projection.
run --import "$work/conversation.jsonl"; ok
[[ -s "$root/stdout" && $(requests) == "$before" ]]
