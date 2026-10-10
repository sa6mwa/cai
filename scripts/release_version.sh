#!/usr/bin/env bash
set -euo pipefail
if [[ $# -gt 2 ]]; then printf 'usage: %s [ROOT] [--require-release]\n' "$0" >&2; exit 2; fi
root=$(cd "${1:-.}" && pwd -P)
require=${2:-}
[[ -z "$require" || "$require" == --require-release ]] || exit 2
semver='(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)([.](0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*))?(\+([0-9A-Za-z-]+([.][0-9A-Za-z-]+)*))?'
fail() { printf 'release identity: %s\n' "$*" >&2; exit 1; }
owned=false
top=$(git -C "$root" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -n "$top" && $(cd "$top" && pwd -P) == "$root" ]]; then owned=true; fi
version=${CAI_VERSION_OVERRIDE:-}
if [[ -n "$require" ]]; then
  [[ "$owned" == true ]] || fail 'final release requires the owning Git checkout'
  [[ -z "$version" ]] || fail 'candidate version overrides are forbidden for final release'
  [[ -z $(git -C "$root" status --porcelain=v1 --untracked-files=all) ]] || fail 'final release requires a clean committed worktree'
fi
if [[ -z "$version" && "$owned" == true ]]; then
  head=$(git -C "$root" rev-parse HEAD)
  tag_lines=$(git -C "$root" for-each-ref --format='%(objecttype) %(objectname) %(*objectname) %(refname) %(symref)' refs/tags | sed 's/  / - /') || fail 'cannot read release tags'
  tags=()
  while read -r type object peeled ref symbolic; do
    tag=${ref#refs/tags/}
    [[ "$tag" =~ ^v[0-9] ]] || continue
    if [[ "$object" == "$head" || "$peeled" == "$head" ]]; then
      [[ "$tag" =~ ^v${semver}$ ]] || fail "invalid exact release tag: $tag"
      [[ "$type" == commit ]] || fail "exact release tag $tag is not lightweight"
      [[ -z "$symbolic" ]] || fail "exact release tag $tag is symbolic"
      tags+=("$tag")
    fi
  done <<<"$tag_lines"
  [[ ${#tags[@]} -le 1 ]] || fail 'multiple exact release tags on HEAD'
  if [[ ${#tags[@]} == 1 ]]; then version=${tags[0]#v}; fi
elif [[ -z "$version" && -f "$root/VERSION" && -f "$root/RELEASE_MANIFEST" ]]; then
  version=$(sed 's/^[[:space:]]*//;s/[[:space:]]*$//' "$root/VERSION")
fi
[[ -z "$require" || -n "$version" ]] || fail 'HEAD must have one exact lightweight vX.Y.Z release tag'
if [[ -n "$require" ]]; then
  git -C "$root" verify-commit HEAD >/dev/null 2>&1 ||
    fail 'final release requires the final amended commit to have a verified signature'
fi
version=${version:-0.0.0}
[[ "$version" =~ ^${semver}$ ]] || fail "invalid version: $version"
printf '%s\n' "$version"
