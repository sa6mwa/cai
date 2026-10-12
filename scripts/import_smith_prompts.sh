#!/usr/bin/env bash
# Explicit source-data import. Ordinary builds use only the committed assets.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
codex_checkout=${1:?usage: import_smith_prompts.sh CODEX_CHECKOUT}
revision=$(git -C "$codex_checkout" rev-parse HEAD)
catalog=codex-rs/models-manager/models.json
git -C "$codex_checkout" diff --quiet HEAD -- "$catalog" || {
  printf 'Prompt import requires a committed Codex model catalog.\n' >&2
  exit 1
}
assets="$root/prompts/smith"
profiles='[]'
for model in gpt-5.6-terra gpt-6-luna gpt-6-sol gpt-6.1-sol gpt-6-astra; do
  name=$model
  models=$(jq -cn --arg model "$model" '[$model]')
  if [[ "$model" == gpt-5.6-terra ]]; then
    name=gpt-5.6
    models='["gpt-5.6-sol","gpt-5.6-terra","gpt-5.6-luna","codex-auto-review"]'
  fi
  source="$assets/$name-codex.md"
  template="$assets/$name-codex-smith-template.md"
  jq -e --arg model "$model" --argjson models "$models" '
    .models as $catalog |
    ($catalog | map(select(.slug == $model)) | .[0].model_messages.instructions_template) as $base |
    all($models[]; . as $id | any($catalog[];
      .slug == $id and .model_messages.instructions_template == $base))' \
    "$codex_checkout/$catalog" >/dev/null || {
      printf 'Prompt group %s contains missing or distinct templates; split the profiles.\n' "$name" >&2
      exit 1
    }
  jq -ejr --arg model "$model" '.models[] | select(.slug == $model) |
    .model_messages.instructions_template | select(type == "string" and length > 0)' \
    "$codex_checkout/$catalog" >"$source"
  jq -ejr --arg model "$model" '.models[] | select(.slug == $model) |
    .model_messages.instructions_template |
    sub("^You are Codex,"; "You are {{agent_identity}},") |
    gsub("As Codex,"; "As {{agent_identity}},") + "\n{{agent_tools}}\n"' \
    "$codex_checkout/$catalog" >"$template"
  source_hash=$(sha256sum "$source" | cut -d' ' -f1)
  template_hash=$(sha256sum "$template" | cut -d' ' -f1)
  profiles=$(jq -cn --argjson profiles "$profiles" --arg name "$name" \
    --argjson models "$models" --arg revision "$revision" \
    --arg source_hash "$source_hash" --arg template_hash "$template_hash" \
    --arg selector "$catalog#$model.model_messages.instructions_template" \
    '$profiles + [{name:$name,models:$models,
      source:{asset:("prompts/smith/"+$name+"-codex.md"),revision:$revision,
              selector:$selector,sha256:$source_hash},
      template:{asset:("prompts/smith/"+$name+"-codex-smith-template.md"),sha256:$template_hash}}]')
done
jq --argjson profiles "$profiles" '
  del(.template,.equivalent_models,.upstream.base) |
  .profiles = $profiles | .fallback_profile = "gpt-5.6"' \
  "$assets/manifest.json" >"$assets/manifest.json.tmp"
mv "$assets/manifest.json.tmp" "$assets/manifest.json"
