#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode=${1:-debug}
cd "$repo_root"

bootlin_toolchain_file="$repo_root/cmake/toolchains/linux-bootlin.cmake"
native_toolchain_file="$repo_root/cmake/toolchains/native-lifecycle.cmake"
aflpp_toolchain_file="$repo_root/cmake/toolchains/linux-aflpp.cmake"

cache_value() {
  local cache=$1
  local key=$2
  [[ -f "$cache" ]] || return 1
  sed -n "s#^${key}:[^=]*=##p" "$cache" | tail -n 1
}

native_target_id() {
  local system processor
  system=$(uname -s)
  processor=$(uname -m)
  case "$system:$processor" in
    Linux:x86_64 | Linux:amd64)
      printf '%s\n' x86_64-linux-gnu
      ;;
    Linux:*)
      printf 'native lifecycle Linux builds require an x86_64 host because the pinned Bootlin compiler collections are x86-hosted; got %s\n' \
        "$processor" >&2
      return 1
      ;;
    Darwin:*)
      printf '\n'
      ;;
    *)
      printf 'unsupported host for native lifecycle build: %s %s\n' \
        "$system" "$processor" >&2
      return 1
      ;;
  esac
}

configure_preset() {
  local preset=$1
  local expected_toolchain=${2:-}
  local target_id=${3:-}
  local require_bootlin_compiler=${4:-1}
  local build_dir="$repo_root/build/$preset"
  local cache="$build_dir/CMakeCache.txt"
  local -a cmake_args=(--preset "$preset")

  if [[ -n "$expected_toolchain" && -f "$cache" ]]; then
    local cached_toolchain cached_target cached_bootlin_root cached_compiler
    cached_toolchain=$(cache_value "$cache" CMAKE_TOOLCHAIN_FILE || true)
    cached_target=$(cache_value "$cache" CPKT_TARGET_ID || true)
    cached_bootlin_root=$(cache_value "$cache" CPKT_BOOTLIN_ROOT || true)
    cached_compiler=$(cache_value "$cache" CMAKE_C_COMPILER || true)
    if [[ "$cached_toolchain" != "$expected_toolchain" ]]; then
      cmake_args=(--fresh "${cmake_args[@]}")
    elif [[ -n "$target_id" && "$cached_target" != "$target_id" ]]; then
      cmake_args=(--fresh "${cmake_args[@]}")
    elif [[ "$require_bootlin_compiler" = 1 &&
            -n "$target_id" &&
            ( -z "$cached_bootlin_root" ||
              "$cached_compiler" != "$cached_bootlin_root"/* ) ]]; then
      cmake_args=(--fresh "${cmake_args[@]}")
    fi
  fi

  cmake "${cmake_args[@]}"
}

case "$mode" in
  debug)
    configure_preset debug "$native_toolchain_file" "$(native_target_id)"
    cmake --build --preset debug
    ;;
  configure-debug)
    configure_preset debug "$native_toolchain_file" "$(native_target_id)"
    ;;
  configure-release)
    configure_preset release "$bootlin_toolchain_file" x86_64-linux-gnu
    ;;
  configure-x86_64-linux-gnu-release)
    configure_preset x86_64-linux-gnu-release "$bootlin_toolchain_file" x86_64-linux-gnu
    ;;
  configure-asan)
    configure_preset asan "$bootlin_toolchain_file" x86_64-linux-gnu
    ;;
  configure-valgrind)
    configure_preset valgrind "$bootlin_toolchain_file" x86_64-linux-gnu
    ;;
  configure-fuzz)
    "$repo_root/scripts/cpkt-aflpp.sh" ensure
    configure_preset fuzz "$aflpp_toolchain_file" x86_64-linux-gnu 0
    ;;
  configure-integration)
    configure_preset integration "$native_toolchain_file" "$(native_target_id)"
    ;;
  configure-coverage)
    configure_preset coverage "$native_toolchain_file" "$(native_target_id)"
    ;;
  host|release)
    configure_preset release "$bootlin_toolchain_file" x86_64-linux-gnu
    cmake --build --preset release
    ;;
  package-source)
    configure_preset x86_64-linux-gnu-release "$bootlin_toolchain_file" x86_64-linux-gnu
    cmake --build --preset x86_64-linux-gnu-release --target cai_package_source
    ;;
  asan)
    configure_preset asan "$bootlin_toolchain_file" x86_64-linux-gnu
    cmake --build --preset asan
    ;;
  valgrind)
    configure_preset valgrind "$bootlin_toolchain_file" x86_64-linux-gnu
    cmake --build --preset valgrind
    ;;
  fuzz)
    "$repo_root/scripts/cpkt-aflpp.sh" ensure
    configure_preset fuzz "$aflpp_toolchain_file" x86_64-linux-gnu 0
    cmake --build --preset fuzz
    ;;
  integration)
    configure_preset integration "$native_toolchain_file" "$(native_target_id)"
    cmake --build --preset integration
    ;;
  coverage)
    configure_preset coverage "$native_toolchain_file" "$(native_target_id)"
    cmake --build --preset coverage
    ;;
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
        '[build] skipping arm64-apple-darwin-release: complete osxcross toolchain not available'
    fi
    for preset in "${presets[@]}"; do
      cmake --fresh --preset "$preset"
      cmake --build --preset "$preset"
    done
    ;;
  *)
    printf 'usage: %s [debug|configure-debug|configure-release|configure-x86_64-linux-gnu-release|configure-asan|configure-valgrind|configure-fuzz|configure-integration|configure-coverage|host|release|package-source|asan|valgrind|fuzz|integration|coverage|release-matrix]\n' "$0" >&2
    exit 2
    ;;
esac
