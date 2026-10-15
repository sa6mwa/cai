#!/usr/bin/env bash
# No CAI runtime, libmdf, model API, or command execution tool is involved.
source "$(dirname "$0")/cli_test_common.sh"
old_columns=${1:-120}; old_rows=${2:-30}
new_columns=${3:-80}; new_rows=${4:-18}
mode=${5:-watch}
variant=${6:-complete}
draft=${7:-late}
tmux_bin=${CAI_TEST_TMUX:-$(command -v tmux)} || fail 'tmux is required for the resize reproducer'
unset TMUX TMUX_PANE
cd "$root"
tmux_run() { "$tmux_bin" -S terminal.sock -f /dev/null "$@"; }
finish() {
  local status=$?
  if [[ $status != 0 && -f "$root/screen" ]]; then cat "$root/screen" >&2; fi
  tmux_run kill-server >/dev/null 2>&1 || true
  cleanup "$status"
}
trap finish EXIT
mkfifo "$root/control"; exec 8<>"$root/control"
{
  printf '#!/usr/bin/env bash\nset -euo pipefail\n'
  for ((i=0;i<80;i++)); do printf 'printf "HISTORY_%03d\\r\\n"\n' "$i"; done
  printf 'exec %q %q %q %q\n' "$cli" "$root/control" "$mode" "$variant"
} >"$root/runner"
tmux_run new-session -d -s repro -x "$old_columns" -y "$old_rows" cat
tmux_run set-option -g status off
tmux_run set-option -g remain-on-exit on
tmux_run set-option -g remain-on-exit-format ''
tmux_run resize-window -x "$old_columns" -y "$old_rows"
tmux_run respawn-pane -k -t repro:0.0 "exec bash '$root/runner'"
visible() {
  tmux_run capture-pane -p -t repro:0.0 >"$root/screen"
  grep -Fq -- "$1" "$root/screen"
}
wait_for visible 'Welcome to resize reproduction'
if [[ "$mode" == retained ]]; then tmux_run send-keys -t repro:0.0 hold Enter; fi
if [[ "$draft" == early ]]; then
  tmux_run send-keys -t repro:0.0 'draft survives resize'
  wait_for visible '> draft survives resize'
fi
printf F >&8
wait_for visible 'RESIZE READY'
cp "$root/screen" "$root/before-resize"
tmux_run resize-window -x "$new_columns" -y "$new_rows"
tmux_run capture-pane -p -t repro:0.0 >"$root/after-resize-before-write"
printf P >&8
wait_for visible 'Worked for'
if [[ "$draft" != early ]]; then tmux_run send-keys -t repro:0.0 'draft survives resize'; fi
wait_for visible '> draft survives resize'
tmux_run capture-pane -p -S - -t repro:0.0 >"$root/transcript"
grep -Fxq '* Edited snake.c' "$root/screen" || fail 'Old source text contaminated the edit heading'
for ((i=0;i<6;i++)); do
  expected=$(printf '%4d +PATCH_ROW_%03d' "$((i+1))" "$i")
  grep -Fxq "$expected" "$root/screen" || fail "Patch row $i was misplaced or overwritten"
done
awk '
  /^> draft survives resize/ {prompt=NR; count++}
  /[^[:space:]]/ {last=NR}
  END {exit !(count == 1 && prompt == last)}' "$root/screen" ||
  fail 'Prompt is misplaced or stale source text remains below it'
tmux_run send-keys -t repro:0.0 C-u /quit Enter
pane_dead() { [[ $(tmux_run display-message -p -t repro:0.0 '#{pane_dead}') == 1 ]]; }
wait_for pane_dead
[[ $(tmux_run display-message -p -t repro:0.0 '#{pane_dead_status}') == 0 ]]
