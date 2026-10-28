#!/usr/bin/env bash
# No inference: prove the status selection and live policy indicator in the UI.
source "$(dirname "$0")/cli_test_common.sh"
scope=${1:-cli}
base=("$cli")
for value in '' ':model' 'model:' 'model::ctx' bogus; do
  run --no-config --status-fields "$value" --list; bad
  contains "$root/stderr" status
  [[ ! -e "$root/state" ]]
done
run --status-fields; bad; contains "$root/stderr" 'requires a value'
peer_start
width=220
selection_args=(--status-fields provider:default:branch)
if [[ "$scope" == host ]]; then
  export CAI_TEST_APP_STATUS_FIELDS=provider:default:branch
  selection_args=()
fi
terminal_start -N "${selection_args[@]}" --no-yolo
# The message file is the terminal's current sticky rows, not redraw history.
wait_for has "$term/message" 'custom : gpt-5.6-luna medium : ctx --'
absent "$term/message" YOLO
send '/yolo on\r'; expect 'YOLO enabled.'
wait_for has "$term/message" 'YOLO'
send '/yolo off\r'; expect 'YOLO disabled.'
no_yolo() { ! grep -Fq YOLO "$term/message"; }
wait_for no_yolo
[[ $(requests) == 0 ]]
terminal_close
if [[ "$scope" == host ]]; then
  unset CAI_TEST_APP_STATUS_FIELDS
  export CAI_TEST_APP_NAME=smith SMITH_STATUS_FIELDS=bogus
  run --no-config --list; bad; contains "$root/stderr" status_fields
  run --no-config --status-fields model --list; ok
  run --dump-config "$root/smith-defaults.ini"; ok
  contains "$root/smith-defaults.ini" '# status_fields = "model:ctx:dir:weekly:crd:yolo:goal"'
  contains "$root/smith-defaults.ini" "# config_dir = \"$root/config/smith\""
fi
