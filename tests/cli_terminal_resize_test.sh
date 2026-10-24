#!/usr/bin/env bash
# Observe actual cells and scrollback in tmux, including its real CPR replies.
source "$(dirname "$0")/cli_test_common.sh"
tmux_bin=${1:-$(command -v tmux)}
mode=${2:-settled}
unset TMUX TMUX_PANE
cd "$root"
tmux_run() { "$tmux_bin" -S terminal.sock -f /dev/null "$@"; }
finish() {
  local status=$?
  tmux_run kill-server >/dev/null 2>&1 || true
  cleanup "$status"
}
trap finish EXIT
peer_start
base=("$cli" -p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-image-generation -C "$work")
if [[ "$mode" == producer ]]; then
  export CAI_TEST_RESIZE_READY="$root/resize-ready" CAI_TEST_RESIZE_RELEASE="$root/resize-release"
  mkfifo "$CAI_TEST_RESIZE_RELEASE"; exec 8<>"$CAI_TEST_RESIZE_RELEASE"
fi
{
  printf "cat > snake.c <<'EOF'\n"
  for ((i=0;i<120;i++)); do
    printf '/* SOURCE_%03d: abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 */\n' "$i"
  done
  printf 'EOF\n'
} >"$root/command"
{
  printf '*** Begin Patch\n*** Update File: snake.c\n@@\n'
  for ((i=0;i<120;i++)); do
    printf -- '-/* SOURCE_%03d: abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 */\n' "$i"
    printf '+/* REPLACEMENT_%03d: abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 */\n' "$i"
  done
  printf '*** End Patch\n'
} >"$root/patch"
plan "$(jq -n --rawfile command "$root/command" --rawfile patch "$root/patch" '{plan:[
  {text:"I will write a standalone file before checking it.",
   tool:"exec_command", arguments:{cmd:$command,yield_time_ms:1000}},
  {pause:true,tool:"apply_patch",arguments:$patch},
  "RESIZE FINAL ANSWER"]}')"
{
  printf '#!/usr/bin/env bash\nset -euo pipefail\n'
  for ((i=0;i<80;i++)); do printf 'printf "HISTORY_%03d\\r\\n"\n' "$i"; done
  printf 'exec '; printf '%q ' "${base[@]}" -N; printf '\n'
} >"$root/runner"
tmux_run new-session -d -s cai -x 120 -y 30 cat
tmux_run set-option -g status off
tmux_run set-option -g remain-on-exit on
tmux_run set-option -g remain-on-exit-format ''
tmux_run resize-window -x 120 -y 30
tmux_run respawn-pane -k -t cai:0.0 "exec bash '$root/runner'"
snapshot() {
  tmux_run capture-pane -p -t cai:0.0 >"$root/screen"
  tmux_run capture-pane -p -S - -t cai:0.0 >"$root/transcript"
}
visible() { snapshot; grep -Fq -- "$1" "$root/screen"; }
wait_for visible 'Welcome to cai'
tmux_run send-keys -t cai:0.0 'create the file' Enter
if [[ "$mode" == producer ]]; then
  wait_for test -s "$CAI_TEST_RESIZE_READY"
  wait_for visible 'RESIZE SINK READY'
else
  wait_for visible 'STREAM in flight'
fi
cp "$root/screen" "$root/before-resize"
tmux_run resize-window -x 80 -y 18
tmux_run send-keys -t cai:0.0 'draft survives resize'
if [[ "$mode" == producer ]]; then printf P >&8; fi
wait_for visible 'STREAM in flight'
wait_for visible '> draft survives resize'
cp "$root/screen" "$root/narrow"
tmux_run resize-window -x 130 -y 36
wait_for visible '> draft survives resize'
cp "$root/screen" "$root/wide"
release
wait_for visible 'RESIZE FINAL ANSWER'
wait_for visible 'Worked for'
snapshot
awk '
  /^> draft survives resize/ {prompt=NR; count++}
  /[^[:space:]]/ {last=NR}
  END {exit !(count == 1 && prompt == last)}' "$root/screen" || {
  cat "$root/screen" >&2
  fail 'Prompt is misplaced or stale text remains below it after resize'
}
tmux_run send-keys -t cai:0.0 C-u /quit Enter
pane_dead() { [[ $(tmux_run display-message -p -t cai:0.0 '#{pane_dead}') == 1 ]]; }
wait_for pane_dead
[[ $(tmux_run display-message -p -t cai:0.0 '#{pane_dead_status}') == 0 ]]
