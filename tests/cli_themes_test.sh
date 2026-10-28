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
tui_themes() {
  local count offset
  cp "$root/config/cai/config.ini" "$root/config-before"
  plan '{"plan":[]}'
  terminal_start -N -t monogreen
  count=$(requests)
  send '/theme\r'; expect 'Current: monogreen:everforest'
  expect 'Softline themes and pairings'; expect 'Libmdf themes'
  expect 'escape-free UTF-8 Markdown'
  for pair in synthwave:synthwave-84 plain:ascii accent:night-owl dracula:dracula gruvbox:gruvbox monochrome:horizon monogreen:everforest outrun:outrun-electric riced:catppuccin-mocha default:default; do
    contains "$term/feed" "$pair"
  done
  send '/theme  RICED:IOSVKEM  \r'; expect 'Theme changed to riced:iosvkem.'
  send '/theme\r'; expect 'Current: riced:iosvkem'
  send '/theme unknown\r'; expect 'unknown softline theme'
  send '/theme riced:\r'; expect 'theme requires a softline name'
  send '/theme riced:bogus\r'; expect 'unknown libmdf theme'
  send '/theme dracula extra\r'; expect 'unknown softline theme'
  send '/theme\r'; expect 'Current: riced:iosvkem'
  [[ $(requests) == "$count" ]]
  send '/theme plain\r'; expect 'Theme changed to plain:ascii.'
  printf 'THEME TOOL CONTENT\n' >"$work/theme.txt"
  plan "$(jq -n --arg sample "$sample" '{plan:[{tool:"read_file",arguments:{path:"theme.txt"}},$sample]}')"
  offset=$(wc -c <"$term/raw")
  prompt 'plain theme response' 'Bold and italic text.'
  tail -c +$((offset+1)) "$term/raw" >"$root/plain-runtime"
  plain_styles "$root/plain-runtime"
  absent "$root/plain-runtime" $'\033[1;96m'
  absent "$root/plain-runtime" $'\033[92m'
  send '/theme dracula\r'; expect 'Theme changed to dracula:dracula.'
  plan "$(jq -n --arg sample "$sample" '{plan:[$sample]}')"
  offset=$(wc -c <"$term/raw")
  prompt 'colored theme response' 'Bold and italic text.'
  tail -c +$((offset+1)) "$term/raw" >"$root/colored-runtime"
  contains "$root/colored-runtime" '38;'
  contains "$root/colored-runtime" $'\033[1m'
  plan '{"plan":[{"pause":true,"tool":"read_file","arguments":{"path":"theme.txt"}},"THEME BOUNDARY DONE"]}'
  offset=$(wc -c <"$term/text")
  send 'read while checking theme guard\r'; expect 'STREAM in flight'
  count=$(requests)
  send '/theme synthwave\033\r'; expect 'S 1. /theme synthwave'
  send '/theme\r'; expect 'Current: dracula:dracula'
  [[ $(requests) == "$count" ]]
  release; expect_turn "$offset" 'THEME BOUNDARY DONE'
  expect 'Theme changed to synthwave:synthwave-84.'
  absent "$term/feed" 'Change themes between turns'
  contains "$root/peer/requests.jsonl" 'THEME TOOL CONTENT'
  send '/new\r'; expect 'New session'
  send '/theme\r'; expect 'Current: synthwave:synthwave-84'
  cmp "$root/config-before" "$root/config/cai/config.ini"
  terminal_close
  export NO_COLOR=1
  terminal_start -N -t monogreen
  send '/theme synthwave\r'; expect 'Theme changed to synthwave:synthwave-84.'
  expect 'NO_COLOR keeps the prompt plain'
  send '/theme\r'; expect 'Current: synthwave:synthwave-84'
  expect 'NO_COLOR is set'
  plan "$(jq -n --arg sample "$sample" '{plan:[$sample]}')"
  offset=$(wc -c <"$term/raw")
  prompt 'no color override after switch' 'Bold and italic text.'
  tail -c +$((offset+1)) "$term/raw" >"$root/no-color-runtime"
  plain_styles "$root/no-color-runtime"
  terminal_close
  unset NO_COLOR
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
  export CAI_TEST_APP_THEME_OFFSET=3 CAI_TEST_APP_SUBAGENT_THEME_OFFSET=-300 CAI_TEST_APP_REVIEW_THEME_OFFSET=1234
  count=$(requests)
  terminal_start -N
  send '/theme-offset\r'; expect 'Parent theme offset: 3.'
  send '/subagent-theme-offset\r'; expect 'Subagent theme offset: -300.'
  send '/review-theme-offset\r'; expect 'Review theme offset: 1234.'
  terminal_close
  [[ $(requests) == "$count" ]]
  unset CAI_TEST_APP_THEME_OFFSET CAI_TEST_APP_SUBAGENT_THEME_OFFSET CAI_TEST_APP_REVIEW_THEME_OFFSET
  # The shared /theme command runs through the full CLI scope below. Hosts
  # verify their own settings/argv/environment boundary without repeating it.
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
tui_themes
