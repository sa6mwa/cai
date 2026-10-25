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
cp "$root/patch" "$root/full-patch"
patch_plan() { plan "$(jq -n --rawfile patch "$root/patch" --arg text "$1" '{plan:[{tool:"apply_patch",arguments:$patch},$text]}')"; }
cp "$root/original" "$work/file.c"
patch_plan 'PATCH DONE'
terminal_start -N
prompt 'apply supplied patch' 'PATCH DONE'
contains "$term/text" '* Edited file.c'
contains "$term/text" '   3 -old'; contains "$term/text" '   3 +new'; contains "$term/text" '   8 +new2'
for marker in between1 before0 after1; do absent "$term/text" "$marker"; done
contains "$term/text" ⋮
contains "$term/raw" $'\033[48;2;74;34;29m'; contains "$term/raw" $'\033[48;2;33;58;43m'
contains "$term/raw" $'\033[91m-\033[97m'; contains "$term/raw" $'\033[92m+\033[97m'
grep -Eq $'\033\\[(0;)?1;96mEdited' "$term/raw"
count=$(requests); send '/output full\r'; expect 'setting applies from the next tool call'; [[ $(requests) == "$count" ]]
cp "$root/original" "$work/file.c"; patch_plan 'FULL PATCH DONE'
prompt 'full patch' 'FULL PATCH DONE'
for marker in between1 before0 after1; do contains "$term/text" "$marker"; done
send '/output compact\r'
# Minimal model patches still need real unchanged neighbors. Each preview is
# scoped to this call, including adjacent hunks, deletion and file edges.
for variant in separated adjacent deletion edges blank; do
  cp "$root/original" "$work/file.c"
  case "$variant" in
    separated) content=$'@@\n-old\n+minimal-new\n@@\n-old2\n+minimal-new2\n' ;;
    adjacent) content=$'@@\n-old\n+adjacent-new\n@@\n-between0\n+adjacent-next\n' ;;
    deletion) content=$'@@\n-old\n' ;;
    edges) content=$'@@\n-before0\n+first-new\n@@\n-after1\n+last-new\n' ;;
    blank)
      printf 'before0\n\nold\n\nafter0\n' >"$work/file.c"
      content=$'@@\n-old\n+blank-new\n' ;;
  esac
  printf '*** Begin Patch\n*** Update File: file.c\n%s*** End Patch\n' "$content" >"$root/patch"
  patch_plan "CONTEXT $variant DONE"
  offset=$(wc -c <"$term/feed")
  prompt "context $variant" "CONTEXT $variant DONE"
  tail -c +$((offset+1)) "$term/feed" | sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' >"$root/preview"
  case "$variant" in
    separated)
      for row in '2  before1' '4  between0' '7  between3' '9  after0' '⋮'; do contains "$root/preview" "$row"; done ;;
    adjacent)
      contains "$root/preview" '2  before1'; contains "$root/preview" '5  between1'
      absent "$root/preview" '4  between0'; absent "$root/preview" '3  old' ;;
    deletion)
      contains "$root/preview" '2  before1'; contains "$root/preview" '3  between0' ;;
    edges)
      contains "$root/preview" '2  before1'; contains "$root/preview" '9  after0' ;;
    blank)
      contains "$root/preview" '2  '; contains "$root/preview" '4  '; contains "$root/preview" '+blank-new' ;;
  esac
done
cp "$root/full-patch" "$root/patch"
printf "printf 'OUT01\\nOUT02\\nOUT03\\nOUT04\\nOUT05\\nOUT06\\nOUT07\\n'\nexit 3\n" >"$work/command.sh"
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"sh command.sh","yield_time_ms":1000}},"COMMAND DONE"]}'
offset=$(wc -c <"$term/text")
prompt 'run failing command' 'COMMAND DONE'
tail -c +$((offset+1)) "$term/text" >"$root/later"
contains "$root/later" OUT05; absent "$root/later" OUT06
contains "$root/later" 'Ran sh command.sh'; contains "$root/later" exit=3
contains "$term/raw" $'\033[91m*\033[0m'
# The committed feed has real block boundaries regardless of terminal redraws.
# Strip only presentation controls for assertions; never alter command bytes.
sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' "$term/feed" >"$root/feed"
awk '/^\* (Running|Ran|Edited|Added|Deleted)/ {if (NR>1 && previous!="") exit 1; seen++} {previous=$0} END {if (seen<4) exit 1}' "$root/feed"
# A preamble in a tool response exercises the assistant-to-tool transition.
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"printf PARTIAL","yield_time_ms":1000},"text":"Assistant before a command"},{"tool":"exec_command","arguments":{"cmd":"true","yield_time_ms":1000}},"SPACING DONE"]}'
prompt 'check spacing' 'SPACING DONE'
sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' "$term/feed" >"$root/feed"
awk '
  /^\* (Running|Ran|Edited|Added|Deleted)/ {
    if (NR>1 && previous!="") exit 1
    seen++
  }
  /^  (PATCH DONE|FULL PATCH DONE|CONTEXT .* DONE|COMMAND DONE|SPACING DONE)$/ {
    if (previous!="") exit 1
    answers++
  }
  {previous=$0}
  END {if (seen<8 || answers<9) exit 1}
' "$root/feed"
contains "$root/feed" PARTIAL
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
