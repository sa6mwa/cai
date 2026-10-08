#!/usr/bin/env bash
# Local delivery only. Publication and ref mutation require engineer direction.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
action=${1:-prepare}
phase=identity
trap 'printf "release phase %s failed at line %s: %s (status %s)\n" "$phase" "$LINENO" "$BASH_COMMAND" "$?" >&2' ERR
case "$action" in
  identity) exec "$root/scripts/release_version.sh" "$root" --require-release ;;
  final) version=$("$root/scripts/release_version.sh" "$root" --require-release) ;;
esac
target_text=$("$root/scripts/release_targets.sh")
mapfile -t targets <<<"$target_text"
build_matrix() {
  local target
  for target in "${targets[@]}"; do
    [[ "$target" != x86_64-linux-gnu || "${source_lane:-0}" == 0 ]] || continue
    phase="build $target"
    if [[ "$action" == configure-matrix ]]; then
      "$root/scripts/build.sh" "configure-$target-release"
    else
      "$root/scripts/build.sh" "$target-release"
    fi
  done
}
case "$action" in
  build-matrix) build_matrix; exit ;;
  configure-matrix) build_matrix; exit ;;
  prepare)
    version=$("$root/scripts/release_version.sh" "$root")
    phase=native
    make test-all
    build_matrix
    native="$root/build/x86_64-linux-gnu-release"
    ctest --test-dir "$native" --no-tests=error --output-on-failure --stop-on-failure --parallel 1 -LE 'live|host'
    ;;
  final)
    # This read-only gate MUST precede clean, tool provisioning and producers.
    phase=clean
    "$root/scripts/clean.sh"
    phase=source-archive
    cmake "-DCAI_ROOT=$root" "-DCAI_DIST_DIR=$root/dist" "-DCAI_VERSION=$version" -P cmake/package_source.cmake
    phase=source-release-lane
    "$root/scripts/test_release_from_source.sh" "$root" "$root/dist/cai-$version.tar.gz"
    native="$root/build/release-source-smoke/build"
    source_lane=1
    # Different instrumentation and facade configurations have separate proof.
    phase=hardening
    make valgrind fuzz-smoke lua-test test-offline
    phase=e2e
    "$root/scripts/run_mcp_everything_service.sh" "$native/cai_mcp_everything_e2e" "$("$root/scripts/devenv.sh" endpoint mcp-everything)"
    build_matrix
    ;;
  *) printf 'usage: %s {identity|configure-matrix|build-matrix|prepare|final}\n' "$0" >&2; exit 2 ;;
esac
if [[ ${CAI_ENABLE_INTEGRATION_TESTS:-0} == 1 ]]; then
  phase=live
  make test-integration
fi
phase=cli
"$root/scripts/build.sh" cai
"$root/scripts/test.sh" cai
phase=packages
for target in "${targets[@]}"; do
  directory="$root/build/$target-release"
  [[ "$target" != x86_64-linux-gnu ]] || directory="$native"
  cmake "-DCAI_ROOT=$root" "-DCAI_BINARY_DIR=$directory" "-DCAI_DIST_DIR=$root/dist" "-DCAI_VERSION=$version" "-DCAI_TARGET_ID=$target" \
    "-DCAI_STRIP_BIN=$(sed -n 's/^CMAKE_STRIP:[^=]*=//p' "$directory/CMakeCache.txt")" -P cmake/package_archive.cmake
done
cmake --build --preset cai --target cai_package_cli
make release-lua-artifacts
if [[ "$action" == prepare ]]; then
  cmake "-DCAI_ROOT=$root" "-DCAI_DIST_DIR=$root/dist" "-DCAI_VERSION=$version" -P cmake/package_source.cmake
fi
phase=checksums
cmake "-DCAI_DIST_DIR=$root/dist" "-DCAI_VERSION=$version" "-DCAI_TARGETS=$(IFS=';'; echo "${targets[*]}")" -P cmake/package_checksums.cmake
phase=final-byte-verification
export CAI_NATIVE_RELEASE_BINARY_DIR="$native"
export CAI_RELEASE_TARGETS
CAI_RELEASE_TARGETS=$(IFS=';'; printf '%s' "${targets[*]}")
"$root/scripts/package-verify.sh" "$root" "$version"
printf 'Verified local artifact inventory: %s\n' "$root/dist/cai-$version-CHECKSUMS"
