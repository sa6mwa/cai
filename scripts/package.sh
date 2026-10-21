#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
[[ $# -le 1 && ${1:-release-matrix} == release-matrix ]] || exit 2
version=$("$root/scripts/release_version.sh" "$root")
target_text=$("$root/scripts/release_targets.sh")
# Warm packaging consumes prepared build trees. Validate the complete selection
# before replacing any archives; explicit preparation owns configure/provisioning.
while read -r target; do
  [[ -f "$root/build/$target-release/CMakeCache.txt" ]] || {
    printf 'package: missing %s build tree; run make build-release first\n' "$target" >&2
    exit 2
  }
done <<<"$target_text"
while read -r target; do
  cmake --build --preset "$target-release" --target cai_package_archive
done <<<"$target_text"
# A binary rehearsal has its own scope; it cannot replace the final manifest.
cmake "-DCAI_DIST_DIR=$root/dist" "-DCAI_VERSION=$version" \
  "-DCAI_TARGETS=${target_text//$'\n'/;}" \
  "-DCAI_PACKAGE_SCOPE=binary" "-DCAI_CHECKSUMS_PATH=$root/build/binary-CHECKSUMS" -P cmake/package_checksums.cmake
