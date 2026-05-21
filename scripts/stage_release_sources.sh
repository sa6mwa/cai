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
tmp_ignored=""
tmp_filtered=""

cleanup() {
  if [[ -n "$tmp_manifest" && -f "$tmp_manifest" ]]; then
    rm -f "$tmp_manifest"
  fi
  if [[ -n "$tmp_ignored" && -f "$tmp_ignored" ]]; then
    rm -f "$tmp_ignored"
  fi
  if [[ -n "$tmp_filtered" && -f "$tmp_filtered" ]]; then
    rm -f "$tmp_filtered"
  fi
}

trap cleanup EXIT INT TERM

rm -rf "$stage_dir"
mkdir -p "$stage_dir"

if git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  tmp_manifest="$(mktemp)"
  tmp_ignored="$(mktemp)"
  tmp_filtered="$(mktemp)"
  git -C "$repo_root" ls-files >"$tmp_manifest"
  git -C "$repo_root" check-ignore --no-index --stdin <"$tmp_manifest" \
    >"$tmp_ignored" 2>/dev/null || true
  if [[ -s "$tmp_ignored" ]]; then
    grep -F -x -v -f "$tmp_ignored" "$tmp_manifest" >"$tmp_filtered"
  else
    cp "$tmp_manifest" "$tmp_filtered"
  fi
  cp "$tmp_filtered" "$stage_dir/RELEASE_MANIFEST"
  tar -C "$repo_root" -cf - -T "$tmp_filtered" | tar -xf - -C "$stage_dir"
elif [[ -f "$manifest_path" ]]; then
  cp "$manifest_path" "$stage_dir/RELEASE_MANIFEST"
  tar -C "$repo_root" -cf - -T "$manifest_path" | tar -xf - -C "$stage_dir"
else
  printf 'stage_release_sources.sh: git worktree or RELEASE_MANIFEST is required\n' >&2
  exit 1
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
