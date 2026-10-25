#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
scope=${1:-full}
base=("$cli")
run --list-themes; ok
for pair in synthwave:synthwave-84 plain:ascii accent:night-owl dracula:dracula gruvbox:gruvbox monochrome:horizon monogreen:everforest outrun:outrun-electric riced:catppuccin-mocha default:default; do
  contains "$root/stdout" "$pair"
done
contains "$root/stdout" 'libmdf themes:'
contains "$root/stdout" iosvkem
absent "$root/stdout" $'\033'
[[ ! -e "$root/config/cai/config.ini" && ! -e "$root/state/cai" ]]

if [[ "$scope" == full ]]; then
  for value in '' bogus ':dracula' 'riced:' 'riced:bogus' 'riced:dracula:default'; do
    run --theme "$value" --list; bad
    contains "$root/stderr" theme
    [[ ! -e "$root/state/cai" ]]
  done
  run -t; bad; contains "$root/stderr" 'requires a value'
  contains "$root/config/cai/config.ini" '# theme = "default"'
  printf '[ui]\ntheme = bogus\n' >"$root/config/cai/config.ini"
  run --list; bad
  export CAI_THEME=plain
  run --list; ok
  export CAI_THEME=bogus
  run --list -t default; ok
  unset CAI_THEME
  printf '[ui]\ntheme = riced:iosvkem\n' >"$root/config/cai/config.ini"
  run --list; ok
else
  # Programmatic receiver settings take effect without theme argv.
  export CAI_TEST_APP_THEME=bogus
  run --list; bad; contains "$root/stderr" 'unknown softline theme'
  export CAI_TEST_APP_THEME=dracula
  export CAI_TEST_APP_INTRODUCTION='## Introduction theme'
fi

peer_start
sample='## Theme sample

**Bold** and *italic* text.'
render() {
  local label=$1
  shift
  plan "$(jq -n --arg sample "$sample" '{plan:[$sample]}')"
  terminal_start -N "$@"
  prompt SAME_PROMPT 'Bold and italic text.'
  cp "$term/feed" "$root/feed-$label"
  # The feed capture deliberately strips ANSI. Compare the raw terminal's
  # palette, independent of how often sticky rows are redrawn.
  { LC_ALL=C grep -oE $'\033\\[[0-9;:]*m' "$term/raw" || :; } |
    LC_ALL=C sort -u >"$root/style-$label"
  terminal_close
}
plain_styles() {
  # Softline still owns its fixed gray message/input styles. Markdown must
  # contribute neither palette colors nor bold/italic decoration.
  absent "$1" '38;'
  absent "$1" '48;'
  absent "$1" $'\033[1m'
  absent "$1" $'\033[3m'
}
if [[ "$scope" != full ]]; then
  render host
  unset CAI_TEST_APP_THEME
  render explicit --theme dracula:dracula
  cmp "$root/style-host" "$root/style-explicit"
  export CAI_TEST_APP_THEME=bogus
  render override -t plain
  plain_styles "$root/style-override"
  unset CAI_TEST_APP_THEME
  export CAI_TEST_APP_NAME=smith SMITH_THEME=bogus
  run --list; bad; contains "$root/stderr" 'unknown softline theme'
  export SMITH_THEME=riced:iosvkem
  run --list; ok
  unset CAI_TEST_APP_NAME SMITH_THEME
  exit 0
fi
render config
render custom -t riced:iosvkem
cmp "$root/style-config" "$root/style-custom"
render alias -t dracula
render explicit --theme dracula:dracula
cmp "$root/style-alias" "$root/style-explicit"
render other -t plain:gruvbox
[[ -s "$root/style-alias" && -s "$root/style-other" ]]
if cmp -s "$root/style-alias" "$root/style-other"; then fail 'Themes did not change output styles'; fi
render plain -t plain
plain_styles "$root/style-plain"
export NO_COLOR=1
render no-color -t synthwave
plain_styles "$root/style-no-color"
unset NO_COLOR

# Themes do not change batch logging or final-response stdout. Grouped -t
# must consume its own argument without disturbing repeated instructions.
plan '{"plan":["First theme reply","Final theme reply"]}'
run -Nnt synthwave -i THEME_FIRST -i THEME_SECOND; ok
[[ $(stdout_text) == 'Final theme reply' ]]
absent "$root/stdout" $'\033'
contains "$root/peer/requests.jsonl" THEME_FIRST
contains "$root/peer/requests.jsonl" THEME_SECOND
