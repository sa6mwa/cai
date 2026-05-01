#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
presets=(
  x86_64-linux-gnu-release
  x86_64-linux-musl-release
  aarch64-linux-gnu-release
  aarch64-linux-musl-release
  armhf-linux-gnu-release
  armhf-linux-musl-release
)

if [ -x "${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}/bin/arm64-apple-darwin25-clang" ]; then
  presets+=(arm64-apple-darwin-release)
else
  printf '[build] skipping arm64-apple-darwin-release: osxcross toolchain not available\n'
fi

for preset in "${presets[@]}"; do
  cmake --preset "$preset"
  cmake --build --preset "$preset"
done
