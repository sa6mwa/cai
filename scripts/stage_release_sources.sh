#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  printf 'usage: %s <repo-root> <stage-dir> [release-version]\n' "$0" >&2
  exit 1
fi

repo_root=$1
stage_dir=$2
release_version=${3:-}
manifest_path="$repo_root/RELEASE_MANIFEST"
tmp_manifest=""

cleanup() {
  if [[ -n "$tmp_manifest" && -f "$tmp_manifest" ]]; then
    rm -f "$tmp_manifest"
  fi
}

trap cleanup EXIT INT TERM

rm -rf "$stage_dir"
mkdir -p "$stage_dir"

if [[ -f "$manifest_path" ]]; then
  cp "$manifest_path" "$stage_dir/RELEASE_MANIFEST"
  tar -C "$repo_root" -cf - -T "$manifest_path" | tar -xf - -C "$stage_dir"
elif git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  tmp_manifest="$(mktemp)"
  git -C "$repo_root" ls-files >"$tmp_manifest"
  cp "$tmp_manifest" "$stage_dir/RELEASE_MANIFEST"
  tar -C "$repo_root" -cf - -T "$tmp_manifest" | tar -xf - -C "$stage_dir"
else
  tar -C "$repo_root" \
    --exclude='./.git' \
    --exclude='./stash' \
    --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.cache' \
    --exclude='./compile_commands.json' \
    -cf - . \
    | tar -xf - -C "$stage_dir"
fi

if [[ -n "$release_version" ]]; then
  printf '%s\n' "$release_version" >"$stage_dir/VERSION"
  if ! grep -qx 'VERSION' "$stage_dir/RELEASE_MANIFEST" 2>/dev/null; then
    printf '%s\n' 'VERSION' >>"$stage_dir/RELEASE_MANIFEST"
  fi
  if ! grep -qx 'RELEASE_MANIFEST' "$stage_dir/RELEASE_MANIFEST" 2>/dev/null; then
    printf '%s\n' 'RELEASE_MANIFEST' >>"$stage_dir/RELEASE_MANIFEST"
  fi
fi
