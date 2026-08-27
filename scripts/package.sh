#!/usr/bin/env bash
set -euo pipefail

mode=${1:-release-matrix}
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${CAI_BUILD_LOCK_HELD:-}" ||
      "${CAI_BUILD_LOCK_HELD}" != "${CAI_BUILD_LOCK_PATH:-}" ]]; then
  exec "$repo_root/scripts/with_build_lock.sh" "$0" "$@"
fi
cd "$repo_root"

case "$mode" in
  release-matrix)
    presets=(
      x86_64-linux-gnu-release
      x86_64-linux-musl-release
      aarch64-linux-gnu-release
      aarch64-linux-musl-release
      armhf-linux-gnu-release
      armhf-linux-musl-release
    )
    if "$repo_root/scripts/osxcross_available.sh" --quiet; then
      presets+=(arm64-apple-darwin-release)
    else
      printf '%s\n' \
        '[package] skipping arm64-apple-darwin-release: complete osxcross toolchain not available'
    fi
    for preset in "${presets[@]}"; do
      cmake --build --preset "$preset" --target cai_package_archive
    done
    "$repo_root/scripts/build.sh" package-source
    ;;
  *)
    printf 'usage: %s [release-matrix]\n' "$0" >&2
    exit 2
    ;;
esac
