#!/usr/bin/env bash
set -euo pipefail
source_root=${1:?source root required}
binary_root=${2:?binary root required}
scratch=$(mktemp -d "$binary_root/prompt-import.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/output/scripts" "$scratch/output/prompts/smith" \
  "$scratch/codex/codex-rs/models-manager"
cp "$source_root/scripts/import_smith_prompts.sh" "$scratch/output/scripts/"
cp "$source_root/prompts/smith/manifest.json" "$scratch/output/prompts/smith/"
catalog="$scratch/codex/codex-rs/models-manager/models.json"
printf '%s\n' '{"models":[]}' >"$catalog"
while IFS= read -r profile; do
  asset=$(jq -r '.source.asset' <<<"$profile")
  models=$(jq -c '.models' <<<"$profile")
  jq --argjson models "$models" --rawfile text "$source_root/$asset" \
    '.models += [$models[] | {slug:.,model_messages:{instructions_template:$text}}]' \
    "$catalog" >"$catalog.tmp"
  mv "$catalog.tmp" "$catalog"
done < <(jq -c '.profiles[]' "$source_root/prompts/smith/manifest.json")
git -C "$scratch/codex" init -q
commit_catalog() {
  git -C "$scratch/codex" add "$catalog"
  git -C "$scratch/codex" -c user.name=Fixture -c user.email=fixture@example.invalid \
    -c commit.gpgsign=false -c core.hooksPath=/dev/null \
    commit -qm 'test(prompts): provide a model catalog'
}
commit_catalog
importer="$scratch/output/scripts/import_smith_prompts.sh"
bash "$importer" "$scratch/codex"
revision=$(git -C "$scratch/codex" rev-parse HEAD)
jq -e --arg revision "$revision" 'all(.profiles[]; .source.revision == $revision)' \
  "$scratch/output/prompts/smith/manifest.json" >/dev/null
while IFS= read -r asset; do
  cmp "$source_root/$asset" "$scratch/output/$asset"
done < <(jq -r '.profiles[] | .source.asset,.template.asset' "$source_root/prompts/smith/manifest.json")
hash=$(sha256sum "$scratch/output/prompts/smith/manifest.json")
printf '\n' >>"$catalog"
if bash "$importer" "$scratch/codex" >"$scratch/stdout" 2>"$scratch/stderr"; then
  printf 'Importer accepted an uncommitted catalog\n' >&2; exit 1
fi
grep -Fq 'committed Codex model catalog' "$scratch/stderr"
[[ $(sha256sum "$scratch/output/prompts/smith/manifest.json") == "$hash" ]]
git -C "$scratch/codex" restore "$catalog"
jq '.models |= map(if .slug == "gpt-5.6-luna" then
  .model_messages.instructions_template += "\nDifferent Luna contract" else . end)' \
  "$catalog" >"$catalog.tmp"
mv "$catalog.tmp" "$catalog"
commit_catalog
if bash "$importer" "$scratch/codex" >"$scratch/stdout" 2>"$scratch/stderr"; then
  printf 'Importer collapsed distinct model instructions\n' >&2; exit 1
fi
grep -Fq 'split the profiles' "$scratch/stderr"
[[ $(sha256sum "$scratch/output/prompts/smith/manifest.json") == "$hash" ]]
