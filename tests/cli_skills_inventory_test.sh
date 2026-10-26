#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
lua_cli=${1:-}
lua_script=${2:-}
peer_start
skill_root="$root/skills"
mkdir -p "$skill_root/alpha" "$skill_root/beta" "$skill_root/odd" "$skill_root/broken" "$root/empty"
printf '%s\n' '---' 'name: alpha-skill' 'description: First loaded description.' '---' 'BODY_NOT_IN_INVENTORY' >"$skill_root/alpha/SKILL.md"
printf '%s\n' '---' 'name: beta_skill' 'description: >-' \
  '  This longer description wraps across a narrow terminal while retaining its complete text.' \
  '  It includes pipes |, *stars*, _underlines_, <tags>, and UTF-8 åäö.' '---' >"$skill_root/beta/SKILL.md"
printf '%s\n' '---' "name: 'odd|_*[]'" 'description: Literal punctuation.' '---' >"$skill_root/odd/SKILL.md"
printf 'invalid front matter\n' >"$skill_root/broken/SKILL.md"
base+=(--skills-dir "$skill_root")
if [[ -n "$lua_cli" ]]; then "$lua_cli" "$lua_script" "$skill_root" "$work"; fi
width=90
terminal_start -N
before=$(requests)
send '/skills\r'; expect 'Name a listed skill'; expect 'alpha-skill'; expect 'beta_skill'; expect 'odd|_*[]'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/inventory"
for text in 'Skill' 'Description' 'First loaded description.' 'UTF-8 åäö.' '*stars*' '_underlines_' '<tags>'; do contains "$root/inventory" "$text"; done
contains "$root/inventory" '┌'
for text in BODY_NOT_IN_INVENTORY broken '\|'; do absent "$root/inventory" "$text"; done
[[ $(requests) == "$before" ]]
send '/help\r'; expect '/skills'; expect 'List loaded skill names and descriptions.'
# The command reports the captured developer catalog, not a fresh disk scan.
printf '%s\n' '---' 'name: changed-name' 'description: CHANGED_AFTER_OPEN' '---' >"$skill_root/alpha/SKILL.md"
mkdir -p "$skill_root/added"
printf '%s\n' '---' 'name: ADDED_AFTER_OPEN' 'description: New package.' '---' >"$skill_root/added/SKILL.md"
plan '{"plan":[{"pause":true,"pause_text":"SKILLS WAITING\n","text":"SKILLS DONE"}]}'
send 'work\r'; expect 'SKILLS'
before=$(requests)
send '/skills\r'; expect 'Name a listed skill'; expect 'alpha-skill'; expect 'First loaded description.'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/busy-inventory"
absent "$root/busy-inventory" CHANGED_AFTER_OPEN
absent "$root/busy-inventory" ADDED_AFTER_OPEN
[[ $(requests) == "$before" ]]
release; expect_turn "$send_offset" 'SKILLS DONE'
terminal_close
before=$(requests)
terminal_start -N --no-skills
send '/skills\r'; expect 'Skills are disabled.'; expect '--skills'
[[ $(requests) == "$before" ]]
terminal_close
terminal_start -N --skills-dir "$root/empty"
send '/skills\r'; expect 'No skills loaded.'; expect '--install-skill DIR'
[[ $(requests) == "$before" ]]
terminal_close
