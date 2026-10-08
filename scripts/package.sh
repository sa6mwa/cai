#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
[[ ${1:-release-matrix} == release-matrix ]] || exit 2
target_text=$("$root/scripts/release_targets.sh")
while read -r target; do
  cmake --build --preset "$target-release" --target cai_package_archive
done <<<"$target_text"
# A binary rehearsal has its own scope; it cannot replace the final manifest.
cmake "-DCAI_DIST_DIR=$root/dist" "-DCAI_VERSION=$("$root/scripts/release_version.sh" "$root")" \
  "-DCAI_TARGETS=${target_text//$'\n'/;}" \
  "-DCAI_PACKAGE_SCOPE=binary" "-DCAI_CHECKSUMS_PATH=$root/build/binary-CHECKSUMS" -P cmake/package_checksums.cmake
