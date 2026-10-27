#!/usr/bin/env bash
# Assert actual terminal rows, including softline's cursor-position handshake.
source "$(dirname "$0")/cli_test_common.sh"
host=$(realpath "$1")
peer_start
# This fixture isolates banner/prompt spacing. Policy-notice placement is
# covered by the replay regression, and should not add another block here.
export LOG_LEVEL=error
plan '{"plan":["REPLAY READY"]}'
run -Nni STARTUP_USER; ok
run --list; ok
session=$(awk 'NF {print $1; exit}' "$root/stdout")
unset TMUX TMUX_PANE
cd "$root"
tmux_run() { "$CAI_TEST_TMUX" -S spacing.sock -f /dev/null "$@"; }
finish() {
  local status=$?
  tmux_run kill-server >/dev/null 2>&1 || true
  cleanup "$status"
}
trap finish EXIT
for mode in shell banner; do
  program="$cli"
  marker='SHELL PROMPT cai'
  unset CAI_TEST_APP_BANNER
  if [[ "$mode" == banner ]]; then
    program="$host"
    export CAI_TEST_APP_BANNER=$'HOST STARTUP BANNER\n'
    marker='HOST STARTUP BANNER'
  fi
  {
    printf '#!/usr/bin/env bash\nset -euo pipefail\n'
    printf "printf 'SHELL PROMPT cai\\r\\n'\n"
    printf 'exec '; printf '%q ' "$program" "${base[@]:1}" --resume "$session"; printf '\n'
  } >"$root/runner"
  tmux_run new-session -d -s spacing -x 100 -y 25 cat
  tmux_run set-option -g status off
  tmux_run set-option -g remain-on-exit on
  tmux_run respawn-pane -k -t spacing:0.0 "exec bash '$root/runner'"
  ready() {
    tmux_run capture-pane -p -t spacing:0.0 >"$root/screen"
    grep -Fq 'Conversation resumed.' "$root/screen"
  }
  wait_for ready
  awk -v marker="$marker" '
    $0 == marker {above=NR}
    /^> STARTUP_USER$/ {found=1; if (!above || NR-above != 2) exit 1}
    END {if (!found) exit 1}' "$root/screen" || {
      cat "$root/screen" >&2; fail 'Expected exactly one blank row before first user prompt';
    }
  tmux_run send-keys -t spacing:0.0 /quit Enter
  dead() { [[ $(tmux_run display-message -p -t spacing:0.0 '#{pane_dead}') == 1 ]]; }
  wait_for dead
  [[ $(tmux_run display-message -p -t spacing:0.0 '#{pane_dead_status}') == 0 ]]
  tmux_run kill-server
done
