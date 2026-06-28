#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
osxcross_root=${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}
osxcross_host=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
presets=(
  x86_64-linux-gnu-release
  x86_64-linux-musl-release
  aarch64-linux-gnu-release
  aarch64-linux-musl-release
  armhf-linux-gnu-release
  armhf-linux-musl-release
)

if [ -x "$osxcross_root/bin/$osxcross_host-clang" ]; then
  presets+=(arm64-apple-darwin-release)
else
  printf '[package] skipping arm64-apple-darwin-release: osxcross toolchain not available at %s\n' \
    "$osxcross_root/bin/$osxcross_host-clang"
fi

for preset in "${presets[@]}"; do
  cmake --build --preset "$preset" --target cai_package_archive
done

cmake --build --preset x86_64-linux-gnu-release --target cai_package_source
