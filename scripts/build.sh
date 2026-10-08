#!/usr/bin/env bash
# CMake presets are authoritative. No preset interpreter or implicit cleaning.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
mode=${1:-debug}
shift "$(( $# > 0 ? 1 : 0 ))"
case "$mode" in
  release-matrix) exec "$root/scripts/release.sh" build-matrix "$@" ;;
  host) mode=release ;;
esac
configure_only=0
case "$mode" in configure-*) configure_only=1; mode=${mode#configure-} ;; esac
case "$mode" in
  debug|debug-cai|libcai|cai|debug-lua|debug-host-deps|release|x86_64-linux-gnu-release|x86_64-linux-musl-release|aarch64-linux-gnu-release|aarch64-linux-musl-release|armhf-linux-gnu-release|armhf-linux-musl-release|arm64-apple-darwin-release|asan|valgrind|coverage|integration|fuzz) ;;
  *) printf 'build: unknown preset %s; see CMakePresets.json\n' "$mode" >&2; exit 2 ;;
esac
[[ "$mode" != fuzz ]] || "$root/scripts/cpkt-aflpp.sh" ensure
# Tool collections and compiler selection stay stable for the workspace. After
# changing either, explicitly remove the affected owned build directory first.
cmake --preset "$mode"
if [[ "$configure_only" == 0 ]]; then
  if [[ $# -gt 0 ]]; then cmake --build --preset "$mode" --target "$@"
  elif [[ "$mode" == debug-cai || "$mode" == cai ]]; then cmake --build --preset "$mode" --target cai_cli_tests
  else cmake --build --preset "$mode"; fi
fi
