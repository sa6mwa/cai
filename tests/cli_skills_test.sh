#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
source_dir="$root/package"
mkdir -p "$source_dir/scripts" "$source_dir/references/example"
cat >"$source_dir/SKILL.md" <<'SKILL'
---
name: installed-example
description: >-
  Help with the installed package
  resources.
---
BODY_ONLY_ON_DEMAND
SKILL
printf '#!/bin/sh\necho SCRIPT_RESOURCE_SENTINEL\n' >"$source_dir/scripts/run.sh"
chmod 755 "$source_dir/scripts/run.sh"
printf 'hidden artifact' >"$source_dir/.artifact"
printf '%s\n' '---' 'name: artifact-not-a-skill' 'description: nested artifact' '---' >"$source_dir/references/example/SKILL.md"
dd if=/dev/zero of="$source_dir/references/binary.dat" bs=65536 count=32 status=none
base=("$cli" -C "$work")
run --install-skill "$source_dir"; ok
installed="$root/config/cai/skills/installed-example"
[[ ! -e "$root/state" ]]
cmp "$source_dir/SKILL.md" "$installed/SKILL.md"
cmp "$source_dir/references/binary.dat" "$installed/references/binary.dat"
[[ $(stat -c %a "$installed/scripts/run.sh") == 700 ]]
run --install-skill "$source_dir"; bad
printf 'changed\n' >>"$source_dir/SKILL.md"
for option in --force --yes; do
  run --install-skill "$source_dir" "$option"; ok
  cmp "$source_dir/SKILL.md" "$installed/SKILL.md"
done
ln -s /etc/passwd "$source_dir/unsafe"
run --install-skill "$source_dir" --force; bad
rm "$source_dir/unsafe"
cmp "$source_dir/SKILL.md" "$installed/SKILL.md"
mkfifo "$source_dir/unsafe"
run --install-skill "$source_dir" --force; bad
rm "$source_dir/unsafe"
run --install-skill "$source_dir" --skills-dir "$source_dir/nested"; bad; [[ ! -e "$source_dir/nested" ]]
mkdir -p "$root/physical/child"
ln -s "$root/physical/child" "$root/ancestor-link"
run --install-skill "$source_dir" --skills-dir "$root/ancestor-link/../skills"; ok
[[ -f "$root/physical/skills/installed-example/SKILL.md" && ! -e "$root/skills" ]]
ln -s "$source_dir" "$root/into-source"
run --install-skill "$source_dir" --skills-dir "$root/into-source/new-root"; bad
[[ ! -e "$source_dir/new-root" ]]
ln -s "$root/physical/skills" "$root/final-link"
run --install-skill "$source_dir" --skills-dir "$root/final-link" --force; bad
run --install-skill "$source_dir" --config-dir "$root/alternate-config"; ok
[[ -f "$root/alternate-config/skills/installed-example/SKILL.md" ]]
mkdir -p "$(dirname "$installed")/invalid-package"
printf 'no front matter\n' >"$(dirname "$installed")/invalid-package/SKILL.md"
peer_start
plan '{"plan":[{"tool":"read_skill","arguments":{"skill":"installed-example"}},{"tool":"read_skill","arguments":{"skill":"installed-example","resource":"scripts/run.sh"}},{"tool":"read_skill","arguments":{"skill":"installed-example","resource":"../../escape"}},"SkillsRuntimeDone"]}'
export LOG_LEVEL=warn
run -Nni 'Use installed-example' --no-review-subagent; ok
contains "$root/stdout" SkillsRuntimeDone
head -1 "$root/peer/requests.jsonl" >"$root/first"
contains "$root/first" 'installed-example: Help with the installed package resources.'
absent "$root/first" BODY_ONLY_ON_DEMAND; absent "$root/first" artifact-not-a-skill
sed -n '2p' "$root/peer/requests.jsonl" >"$root/second"; contains "$root/second" BODY_ONLY_ON_DEMAND
sed -n '3p' "$root/peer/requests.jsonl" >"$root/third"; contains "$root/third" SCRIPT_RESOURCE_SENTINEL
sed -n '4p' "$root/peer/requests.jsonl" >"$root/fourth"; contains "$root/fourth" 'skill resource path is invalid'
contains "$root/stderr" invalid-package; contains "$root/stderr" 'front matter'
