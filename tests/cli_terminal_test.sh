#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
mkdir "$root/clipboard"
"${CAI_TEST_PYTHON:-python3}" "$support/fixtures/clipboard_peer.py" "$root/clipboard" >"$root/clipboard/stdout" 2>"$root/clipboard/stderr" & clipboard_pid=$!
# This test owns the data peer, so always stop it alongside the provider fixture.
finish() { local failure=$?; kill "$clipboard_pid" 2>/dev/null || true; wait "$clipboard_pid" 2>/dev/null || true; cleanup "$failure"; }
trap finish EXIT
wait_for test -s "$root/clipboard/display"
export DISPLAY="$(cat "$root/clipboard/display")" XAUTHORITY="$root/no-authority"
terminal_start -N
absent "$term/text" 'Cai Smith. Enter sends'
probes=$(grep -ao $'\033\[6n' "$term/raw" | wc -l || true)
# A responsive terminal permits startup observation and native-surface
# acquisition. The previous fixture timed out once and disabled all probes.
[[ "$probes" -ge 1 && "$probes" -le 2 ]]
began=$SECONDS
send '/help\r'; expect 'Run cai --help'
[[ $((SECONDS-began)) -lt 2 ]]
for marker in 'CAI help' 'Slash commands' 'Prompt keys' '/resume NUMBER' '/export [ID]' Ctrl-V; do contains "$term/text" "$marker"; done
absent "$term/text" '| --- |'
[[ $(requests) == 0 ]]
send 'inspect this \026'
paste_dir="$root/cache/cai/clipboard"
clipboard_exists() { [[ -d "$paste_dir" ]] && compgen -G "$paste_dir/*.png" >/dev/null; }
wait_for clipboard_exists
image=$(find "$paste_dir" -type f -name '*.png' -print -quit)
expect "$image"
[[ $(stat -c %a "$image") == 600 && $(stat -c %a "$paste_dir") == 700 ]]
[[ ! -d "$root/cache/softline" ]]
printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aAn8AAAAASUVORK5CYII=' | base64 -d >"$root/expected.png"
cmp "$image" "$root/expected.png"
plan "$(jq -n --arg path "$image" '{plan:[{pause:true,reasoning:["Checking the pasted input"],tool:"view_image",arguments:{path:$path}},"FINAL paragraph."]}')"
send '\r'; expect 'STREAM in flight'
absent "$term/text" 'FINAL paragraph.'
count=$(requests)
send '/help\r'; expect 'Run cai --help'; [[ $(requests) == "$count" ]]
send 'preserved-draft'; resize 48; expect_prompt preserved-draft
release; expect_assistant 'FINAL paragraph.'; expect 'Worked for'
contains "$root/peer/requests.jsonl" 'data:image/png;base64,'
contains "$term/text" '! Worked for'
plan '{"plan":["DRAFT SENT DONE"]}'
offset=$(wc -c <"$term/text")
send '\r'; expect_turn "$offset" 'DRAFT SENT DONE'
tail -1 "$root/peer/requests.jsonl" >"$root/draft-request"
contains "$root/draft-request" preserved-draft
terminal_close
saved_cache=$XDG_CACHE_HOME
unset XDG_CACHE_HOME
printf '\377\330\377\340\000\020JFIF\000fixture\377\331' >"$root/expected.jpeg"
jq -n --arg bytes "$(base64 -w0 "$root/expected.jpeg")" '{image_type:"image/jpeg",payload:$bytes}' >"$root/clipboard/config.json"
terminal_start -N
send 'jpeg draft \026'
fallback_clipboard() { compgen -G "$root/.cache/cai/clipboard/*.jpeg" >/dev/null; }
wait_for fallback_clipboard
jpeg=$(find "$root/.cache/cai/clipboard" -name '*.jpeg' -print -quit)
cmp "$jpeg" "$root/expected.jpeg"
[[ $(stat -c %a "$jpeg") == 600 && ! -e "$root/.cache/softline" ]]
plan '{"plan":["FINAL paragraph."]}'
send '\r'; expect 'FINAL paragraph.'; expect 'Worked for'; terminal_close
export XDG_CACHE_HOME="$saved_cache"
printf '%s\n' '{"image_type":"image/png"}' >"$root/clipboard/config.json"
count=$(requests)
terminal_start
expect 'FINAL paragraph.'; [[ $(requests) == "$count" ]]
send '/new\r'; expect '--- session ---'
plan '{"plan":["__transport_failure__"]}'
send 'trigger a transport failure\r'; expect 'HTTP transport failed'
expect 'method: POST'; expect 'path: responses'
absent "$term/text" cai.http.transport_error
terminal_close
# Fragmented links and table escapes must be rendered, not passed through.
export CAI_TEST_VOLATILE_SESSION=1
width=24
plan '{"plan":[["abcdefghijklmnopqrst ([la","bel](https://","x.test", ")).\n\n","| Left | Right |\n| --- | --- |\n| `a\\","|b` | ok |\n\n","MARKDOWN DONE\n\n"]]}'
terminal_start -N
send 'render markdown\r'; expect 'MARKDOWN DONE'; expect 'Worked for'
contains "$term/text" label; contains "$term/text" '(https://x.test))'; contains "$term/text" 'a|b'; absent "$term/text" 'a\|b'
terminal_close
# Parenthesized link destinations must survive whole and fragmented deltas.
# These use the real runtime/libmdf/softline path against a local provider.
width=120
for chunks in \
  '["[DOI](https://doi.org/10.1016/S0031-3203(01)00020-5)\n\n[Escaped](https://example.test/a\\(b\\))\n\nLINK DONE"]' \
  '["[DOI](https://doi.org/10.1016/S0031-3203(","01",")00020-5",")\n\n[Escaped](https://example.test/a\\","(b\\","))\n\nLINK DONE"]'; do
  plan "{\"plan\":[$chunks]}"
  terminal_start -N
  send 'render parenthesized links\r'; expect 'LINK DONE'; expect 'Worked for'
  contains "$term/feed" 'DOI (https://doi.org/10.1016/S0031-3203(01)00020-5)'
  contains "$term/feed" 'Escaped (https://example.test/a(b))'
  absent "$term/feed" '[DOI]('
  absent "$term/feed" '[Escaped]('
  terminal_close
done
width=24
# Image capability boundaries: workspace and clipboard, no siblings/escapes.
cp "$image" "$work/allowed.png"
cp "$image" "$root/cache/cai/other.png"
mkdir "$root/cache/cai/clipboard-other"
cp "$image" "$root/cache/cai/clipboard-other/image.png"
ln -s "$root/cache/cai/other.png" "$paste_dir/escape.png"
cp "$image" "$work/hardlink-source.png"
ln "$work/hardlink-source.png" "$paste_dir/hardlink.png"
for path in "$work/allowed.png" "$root/cache/cai/other.png" "$root/cache/cai/clipboard-other/image.png" "$paste_dir/escape.png" "$paste_dir/hardlink.png"; do
  plan "$(jq -n --arg path "$path" '{plan:[{tool:"view_image",arguments:{path:$path}},"IMAGE DONE"]}')"
  run -Nni 'inspect an image'; ok
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  if [[ "$path" == "$work/allowed.png" ]]; then contains "$root/request" 'data:image/png;base64,'
  else absent "$root/request" 'data:image/png;base64,'; contains "$root/request" error; fi
done
rm "$paste_dir/escape.png" "$paste_dir/hardlink.png"
mv "$paste_dir" "$paste_dir-saved"; ln -s "$paste_dir-saved" "$paste_dir"
run -Nni 'should not run'; bad; [[ ! -s "$root/stdout" ]]; contains "$root/stderr" 'must not be a symlink'
rm "$paste_dir"; mv "$paste_dir-saved" "$paste_dir"
report=$(jq -c . "$support/fixtures/review.json")
for mode in direct subagent; do
  items=$(jq -n --arg path "$image" --arg report "$report" '[{tool:"view_image",arguments:{path:$path}},$report]')
  flags=(--review -i 'Inspect the pasted image')
  if [[ "$mode" == subagent ]]; then
    items=$(jq -n --argjson items "$items" '[{tool:"run_review",arguments:{target:"custom",instructions:"Inspect the pasted image"}}]+$items+["IMAGE REVIEW DONE"]')
    flags=(-Nni 'Review the pasted image')
  fi
  plan "{\"plan\":$items}"
  before=$(requests)
  run "${flags[@]}"; ok
  after=$(requests)
  sed -n "$((before+1)),$((after))p" "$root/peer/requests.jsonl" >"$root/image-review"
  contains "$root/image-review" 'data:image/png;base64,'
done
for config in '{"image_type":null}' '{"image_type":"image/png","payload":"bm90IGFuIGltYWdl"}'; do
  printf '%s\n' "$config" >"$root/clipboard/config.json"
  count=$(find "$paste_dir" -type f | wc -l)
  plan '{"plan":["UNCHANGED DRAFT DONE"]}'
  terminal_start -N
  send 'unchanged-draft\026\r'; expect 'UNCHANGED DRAFT DONE'
  tail -1 "$root/peer/requests.jsonl" >"$root/draft-request"
  contains "$root/draft-request" unchanged-draft
  [[ $(find "$paste_dir" -type f | wc -l) == "$count" ]]
  terminal_close
done
[[ ! -s "$root/clipboard/stderr" ]]
