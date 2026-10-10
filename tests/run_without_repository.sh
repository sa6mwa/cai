#!/usr/bin/env bash
# Keep markerless-workspace fixtures in owned build scratch while excluding
# the surrounding developer repository from their filesystem view.
set -euo pipefail
source_root=$1
binary=$2
shift 2
git_root=$(git -C "$source_root" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z "$git_root" ]]; then exec "$binary" "$@"; fi
if [[ $(uname -s) == Darwin ]]; then
  command -v sandbox-exec >/dev/null || { printf 'sandbox-exec is required for the Darwin markerless-workspace fixture\n' >&2; exit 2; }
  marker="$git_root/.git"
  marker=${marker//\\/\\\\}
  marker=${marker//\"/\\\"}
  exec sandbox-exec -p "(version 1)(allow default)(deny file-read-metadata (literal \"$marker\"))" "$binary" "$@"
fi
command -v bwrap >/dev/null || { printf 'bubblewrap is required for the Linux markerless-workspace fixture\n' >&2; exit 2; }
args=(--die-with-parent --ro-bind / / --dev /dev --proc /proc --tmpfs "$git_root" --bind "$git_root/build" "$git_root/build")
if [[ -d "$git_root/.cache" ]]; then args+=(--ro-bind "$git_root/.cache" "$git_root/.cache"); fi
exec bwrap "${args[@]}" --chdir "$(pwd -P)" "$binary" "$@"
