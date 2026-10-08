#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
base=("$cli" -p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-image-generation -C "$work")
printf 'before0\nbefore1\nold\nbetween0\nbetween1\nbetween2\nbetween3\nold2\nafter0\nafter1\n' >"$root/original"
cat >"$root/patch" <<'PATCH'
*** Begin Patch
*** Update File: file.c
@@
 before0
 before1
-old
+new
 between0
 between1
 between2
 between3
-old2
+new2
 after0
 after1
*** End Patch
PATCH
patch_plan() { plan "$(jq -n --rawfile patch "$root/patch" --arg text "$1" '{plan:[{tool:"apply_patch",arguments:$patch},$text]}')"; }
cp "$root/original" "$work/file.c"
patch_plan 'PATCH DONE'
terminal_start -N
prompt 'apply supplied patch' 'PATCH DONE'
contains "$term/text" '* Edited file.c'
contains "$term/text" '   3 -old'; contains "$term/text" '   3 +new'; contains "$term/text" '   8 +new2'
for marker in between1 before0 after1; do absent "$term/text" "$marker"; done
contains "$term/text" ⋮
contains "$term/raw" $'\033[41m'; contains "$term/raw" $'\033[42m'
grep -Eq $'\033\\[(0;)?1;96mEdited' "$term/raw"
count=$(requests); send '/output full\r'; expect 'setting applies from the next tool call'; [[ $(requests) == "$count" ]]
cp "$root/original" "$work/file.c"; patch_plan 'FULL PATCH DONE'
prompt 'full patch' 'FULL PATCH DONE'
for marker in between1 before0 after1; do contains "$term/text" "$marker"; done
send '/output compact\r'
printf "printf 'OUT01\\nOUT02\\nOUT03\\nOUT04\\nOUT05\\nOUT06\\nOUT07\\n'\nexit 3\n" >"$work/command.sh"
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"sh command.sh","yield_time_ms":1000}},"COMMAND DONE"]}'
offset=$(wc -c <"$term/text")
prompt 'run failing command' 'COMMAND DONE'
tail -c +$((offset+1)) "$term/text" >"$root/later"
contains "$root/later" OUT05; absent "$root/later" OUT06
contains "$root/later" 'Ran sh command.sh'; contains "$root/later" exit=3
contains "$term/raw" $'\033[91m*\033[0m'
terminal_close
cp "$root/original" "$work/file.c"; patch_plan 'FINAL ONLY'
run -Nni 'apply patch'; ok
[[ $(stdout_text) == 'FINAL ONLY' ]]
jq -se 'any(.[]; .event == "tool_patch")' "$root/stderr" >/dev/null
for marker in between1 before0 after1; do contains "$root/stderr" "$marker"; done
absent "$root/stderr" $'\033'
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"sh command.sh","yield_time_ms":1000}},"LAST RESPONSE"]}'
run -Nni 'run command' --tool-output compact; ok
[[ $(stdout_text) == 'LAST RESPONSE' ]]
contains "$root/stderr" OUT06; contains "$root/stderr" OUT07; jq -e . "$root/stderr" >/dev/null
run --tool-output invalid; bad; contains "$root/stderr" 'compact or full'
