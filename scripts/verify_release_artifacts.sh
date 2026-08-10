#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--self-test" ]]; then
  repo_root=${CAI_REPO_ROOT:-$(pwd)}
  version=${CAI_VERSION:-0.0.0}
  self_test=1
elif [[ $# -lt 1 || $# -gt 2 ]]; then
  printf 'usage: %s <repo-root> [version]\n' "$0" >&2
  exit 1
else
  repo_root=$1
  version=${2:-}
  self_test=0
fi

if [[ -z "$version" ]]; then
  version=$(sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' \
    "$repo_root/build/x86_64-linux-gnu-release/generated/include/cai/version.h" \
    2>/dev/null || true)
fi
if [[ -z "$version" ]]; then
  printf 'verify_release_artifacts.sh: version is required\n' >&2
  exit 1
fi

dist_dir="$repo_root/dist"
checksums="$dist_dir/cai-$version-CHECKSUMS"

fail() {
  printf 'verify_release_artifacts.sh: %s\n' "$*" >&2
  exit 1
}

host_home=${HOME:-}

require_file() {
  local path=$1
  [[ -f "$path" ]] || fail "missing required file: $path"
}

require_member() {
  local listing=$1
  local member=$2
  grep -qx "$member" "$listing" || fail "archive missing member: $member"
}

require_no_member() {
  local listing=$1
  local member=$2
  if grep -qx "$member" "$listing"; then
    fail "archive contains forbidden member: $member"
  fi
}

require_no_member_glob() {
  local listing=$1
  local pattern=$2
  local match

  match=$(grep -E "$pattern" "$listing" || true)
  if [[ -n "$match" ]]; then
    printf '%s\n' "$match" >&2
    fail "archive contains forbidden member matching: $pattern"
  fi
}

verify_listing_has_no_host_paths() {
  local listing=$1
  local matches

  matches=$(grep -E '/home/|/Users/|/opt/|\.cache/deps' "$listing" || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "archive member list contains host-specific paths"
  fi
  matches=$(grep -F "$repo_root" "$listing" || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "archive member list contains repository path"
  fi
  if [[ -n "$host_home" ]]; then
    matches=$(grep -F "$host_home" "$listing" || true)
    if [[ -n "$matches" ]]; then
      printf '%s\n' "$matches" >&2
      fail "archive member list contains HOME path"
    fi
  fi
}

verify_no_private_bytes() {
  local root_dir=$1
  local file
  local matches

  while IFS= read -r file; do
    if [[ -n "$host_home" ]]; then
      matches=$(strings -a "$file" 2>/dev/null | grep -n -F "$host_home" || true)
      if [[ -n "$matches" ]]; then
        printf '%s:%s\n' "$file" "$matches" >&2
        fail "artifact contains HOME path"
      fi
    fi
    matches=$(strings -a "$file" 2>/dev/null | grep -n -F "$repo_root" || true)
    if [[ -n "$matches" ]]; then
      printf '%s:%s\n' "$file" "$matches" >&2
      fail "artifact contains repository path"
    fi
  done < <(find "$root_dir" -type f -print)
}

release_expected_manifest() {
  local output=$1
  local ignored

  git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || return 1
  ignored=$(mktemp)
  git -C "$repo_root" ls-files >"$output"
  git -C "$repo_root" check-ignore --no-index --stdin <"$output" \
    >"$ignored" 2>/dev/null || true
  if [[ -s "$ignored" ]]; then
    grep -F -x -v -f "$ignored" "$output" >"${output}.filtered"
    mv "${output}.filtered" "$output"
  fi
  rm -f "$ignored"
  printf '%s\n' VERSION RELEASE_MANIFEST >>"$output"
  sort -u -o "$output" "$output"
}

verify_source_matches_git_manifest() {
  local root=$1
  local listing=$2
  local actual
  local expected
  local ignored

  git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || return 0
  actual=$(mktemp)
  expected=$(mktemp)
  ignored=$(mktemp)
  sed "s#^$root/##" "$listing" | grep -v -E '^$|/$' | sort -u >"$actual"
  release_expected_manifest "$expected" || {
    rm -f "$actual" "$expected" "$ignored"
    return 0
  }
  if ! diff -u "$expected" "$actual" >&2; then
    rm -f "$actual" "$expected" "$ignored"
    fail "source archive does not match git-tracked non-ignored manifest"
  fi
  git -C "$repo_root" check-ignore --no-index --stdin <"$actual" \
    >"$ignored" 2>/dev/null || true
  if [[ -s "$ignored" ]]; then
    grep -F -x -v -e VERSION -e RELEASE_MANIFEST "$ignored" \
      >"${ignored}.filtered" || true
    mv "${ignored}.filtered" "$ignored"
  fi
  if [[ -s "$ignored" ]]; then
    cat "$ignored" >&2
    rm -f "$actual" "$expected" "$ignored"
    fail "source archive contains git-ignored paths"
  fi
  rm -f "$actual" "$expected" "$ignored"
}

verify_no_private_text() {
  local root_dir=$1
  local matches

  matches=$(grep -R -I -n -E '/home/|/Users/|/opt/|\.cache/deps|\.\./' \
    "$root_dir" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "artifact contains host-specific or out-of-repository paths"
  fi
}

verify_checksum_file() {
  require_file "$checksums"
  if compgen -G "$dist_dir/cai-$version-SHA256SUMS" >/dev/null; then
    fail "found deprecated SHA256SUMS file; expected CHECKSUMS"
  fi
  verify_no_private_manifest_paths "$checksums"
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$dist_dir" && sha256sum -c "$(basename "$checksums")" >/dev/null)
  elif command -v shasum >/dev/null 2>&1; then
    (cd "$dist_dir" && shasum -a 256 -c "$(basename "$checksums")" >/dev/null)
  else
    fail "neither sha256sum nor shasum is available"
  fi
}

verify_no_private_manifest_paths() {
  local manifest=$1
  local matches

  matches=$(grep -n -E 'file://|/home/|/Users/|/opt/|\.cache/deps|\.\./' \
    "$manifest" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "checksum manifest contains local or non-relocatable paths"
  fi
  if [[ -n "$host_home" ]]; then
    matches=$(grep -n -F "$host_home" "$manifest" 2>/dev/null || true)
    if [[ -n "$matches" ]]; then
      printf '%s\n' "$matches" >&2
      fail "checksum manifest contains HOME path"
    fi
  fi
  matches=$(grep -n -F "$repo_root" "$manifest" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "checksum manifest contains repository path"
  fi
}

read_checksum_artifacts() {
  local line
  local artifact

  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -n "$line" ]] || continue
    artifact=${line#* }
    artifact=${artifact# }
    artifact=${artifact#\*}
    if [[ -z "$artifact" || "$artifact" == "$line" ]]; then
      fail "malformed checksum manifest line: $line"
    fi
    if [[ "$artifact" == /* || "$artifact" == *'/'* || "$artifact" == *'..'* ]]; then
      fail "checksum manifest artifact must be a dist-local filename: $artifact"
    fi
    printf '%s\n' "$artifact"
  done <"$checksums"
}

artifact_list_contains() {
  local needle=$1
  shift
  local artifact

  for artifact in "$@"; do
    if [[ "$artifact" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

verify_manifest_artifacts() {
  local artifact
  local artifact_path

  for artifact in "$@"; do
    artifact_path="$dist_dir/$artifact"
    require_file "$artifact_path"
    case "$artifact" in
      cai-"$version".tar.gz | cai-"$version"-*.tar.gz | cai-lua-"$version".tar.gz | cai-"$version"-*.rockspec | cai-"$version"-*.src.rock)
        ;;
      *)
        fail "checksum manifest lists unexpected release artifact: $artifact"
        ;;
    esac
  done
}

verify_dist_manifest_closure() {
  local artifacts=("$@")
  local release_file
  local release_name

  shopt -s nullglob
  for release_file in \
    "$dist_dir"/cai-*.tar.gz \
    "$dist_dir"/cai-lua-*.tar.gz \
    "$dist_dir"/cai-*.rockspec \
    "$dist_dir"/cai-*.src.rock \
    "$dist_dir"/cai-*-CHECKSUMS \
    "$dist_dir"/cai-*-SHA256SUMS; do
    release_name=$(basename "$release_file")
    case "$release_name" in
      cai-"$version"-CHECKSUMS)
        ;;
      cai-*-CHECKSUMS)
        fail "stale checksum manifest remains in dist: $release_name"
        ;;
      cai-*-SHA256SUMS)
        fail "deprecated checksum manifest remains in dist: $release_name"
        ;;
      cai-"$version".tar.gz | cai-"$version"-*.tar.gz | cai-lua-"$version".tar.gz | cai-"$version"-*.rockspec | cai-"$version"-*.src.rock)
        if ! artifact_list_contains "$release_name" "${artifacts[@]}"; then
          fail "release artifact is not listed in checksum manifest: $release_name"
        fi
        ;;
      *)
        fail "stale release artifact remains in dist: $release_name"
        ;;
    esac
  done
}

verify_single_root() {
  local archive=$1
  local root=$2
  local listing=$3
  local roots

  roots=$(awk -F/ 'NF > 0 && $1 != "" {print $1}' "$listing" | sort -u)
  if [[ "$roots" != "$root" ]]; then
    printf 'archive: %s\nexpected root: %s\nactual roots:\n%s\n' \
      "$archive" "$root" "$roots" >&2
    fail "archive top-level directory mismatch"
  fi
}

verify_no_host_paths() {
  local root_dir=$1
  local matches

  matches=$(grep -R -I -n -E '/home/|/Users/|/opt/|\.cache/deps' \
    "$root_dir/lib" "$root_dir/share" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "release metadata contains host-specific paths"
  fi
}

verify_no_sanitizer_artifacts() {
  local root_dir=$1
  local matches

  matches=$(grep -R -I -n -E 'fsanitize|__asan|__ubsan|__tsan|__msan|libasan|libubsan|libtsan|libmsan' \
    "$root_dir/lib" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "release library contains sanitizer artifact"
  fi
}

verify_dependency_manifest() {
  local root_dir=$1
  local target_id=$2
  local manifest="$root_dir/share/cai/dependencies.json"
  local pc="$root_dir/lib/pkgconfig/cai.pc"
  local manifest_text
  local pc_lonejson_version
  local pc_lonejson_sha256

  require_file "$manifest"
  manifest_text=$(cat "$manifest")
  pc_lonejson_version=$(sed -n 's/^lonejson_version=//p' "$pc")
  pc_lonejson_sha256=$(sed -n 's/^lonejson_sha256=//p' "$pc")
  if ! grep -F '"name": "c.pkt.systems"' <<<"$manifest_text" >/dev/null ||
     ! grep -F '"name": "lonejson"' <<<"$manifest_text" >/dev/null ||
     ! grep -F '"name": "libpslog"' <<<"$manifest_text" >/dev/null ||
     ! grep -F "\"targetId\": \"$target_id\"" <<<"$manifest_text" >/dev/null ||
     ! grep -F '"installRole": "external-static-consumer-sdk"' \
       <<<"$manifest_text" >/dev/null ||
     ! grep -F '"installRole": "public-link-interface"' \
       <<<"$manifest_text" >/dev/null ||
     ! grep -F '"installRole": "public-header-logger-api"' \
       <<<"$manifest_text" >/dev/null; then
    printf '%s\n' "$manifest_text" >&2
    fail "dependency manifest does not describe required SDK dependencies"
  fi
  if [[ -n "$pc_lonejson_version" ]] &&
     ! grep -F "\"version\": \"$pc_lonejson_version\"" \
       <<<"$manifest_text" >/dev/null; then
    printf '%s\n' "$manifest_text" >&2
    fail "dependency manifest lonejson version disagrees with pkg-config"
  fi
  if [[ -n "$pc_lonejson_sha256" ]] &&
     ! grep -F "\"sha256\": \"$pc_lonejson_sha256\"" \
       <<<"$manifest_text" >/dev/null; then
    printf '%s\n' "$manifest_text" >&2
    fail "dependency manifest lonejson checksum disagrees with pkg-config"
  fi
  if grep -E '/home/|/Users/|/opt/|\.cache/deps|file://|\.\./' \
    <<<"$manifest_text" >/dev/null; then
    printf '%s\n' "$manifest_text" >&2
    fail "dependency manifest contains local or non-relocatable paths"
  fi
}

verify_linux_runpath() {
  local root_dir=$1
  local target_id=$2
  local so
  local dynamic
  local readelf

  readelf=$(find_target_readelf "$target_id") || diagnostic_fail \
    package-verify elf-loader-metadata external-tool-unavailable \
    "target-correct readelf is required to verify ELF runtime metadata" \
    "$root_dir" \
    "configure $target_id-release or set CAI_READELF to a target-capable readelf"
  while IFS= read -r so; do
    [[ -L "$so" ]] && continue
    dynamic=$("$readelf" -d "$so" 2>/dev/null || true)
    [[ -n "$dynamic" ]] || continue
    if ! grep -E 'RPATH|RUNPATH' <<<"$dynamic" | grep -F '[$ORIGIN]' >/dev/null; then
      printf '%s\n' "$dynamic" >&2
      fail "shared library does not use \$ORIGIN rpath/runpath: $so"
    fi
    if grep -E 'RPATH|RUNPATH' <<<"$dynamic" | \
      grep -E '/home/|/Users/|/opt/|\.cache/deps' \
        >/dev/null; then
      printf '%s\n' "$dynamic" >&2
      fail "shared library has host-specific runpath: $so"
    fi
    if grep -E 'NEEDED' <<<"$dynamic" | grep -E 'libasan|libubsan|libtsan|libmsan' \
      >/dev/null; then
      printf '%s\n' "$dynamic" >&2
      fail "shared library links sanitizer runtime: $so"
    fi
  done < <(find "$root_dir/lib" -maxdepth 1 -type f -name 'libcai.so*' -print)
}

diagnostic_fail() {
  local surface=$1
  local phase=$2
  local class=$3
  local reason=$4
  local artifact=${5:-}
  local next=${6:-}

  {
    printf 'PKT_DIAGNOSTIC_BEGIN\n'
    printf 'surface=%s\n' "$surface"
    printf 'phase=%s\n' "$phase"
    printf 'status=failed\n'
    printf 'class=%s\n' "$class"
    printf 'reason=%s\n' "$reason"
    [[ -z "$artifact" ]] || printf 'artifact=%s\n' "$artifact"
    [[ -z "$next" ]] || printf 'next=%s\n' "$next"
    printf 'PKT_DIAGNOSTIC_END\n'
  } >&2
  fail "$reason"
}

shell_value() {
  local assignments=$1
  local key=$2
  local line

  line=$(grep -E "^${key}=" <<<"$assignments" | tail -n 1 || true)
  [[ -n "$line" ]] || return 1
  printf '%s\n' "${line#*=}" | xargs printf '%s\n'
}

find_target_otool() {
  local target_id=$1
  local assignments
  local otool

  assignments=$("$repo_root/scripts/discover_target_tools.sh" \
    "$repo_root/build/$target_id-release" "$target_id")
  otool=$(shell_value "$assignments" OTOOL || true)
  if [[ -n "$otool" && -x "$otool" ]]; then
    printf '%s\n' "$otool"
    return 0
  fi
  return 1
}

find_target_readelf() {
  local target_id=$1
  local assignments
  local readelf

  assignments=$("$repo_root/scripts/discover_target_tools.sh" \
    "$repo_root/build/$target_id-release" "$target_id")
  readelf=$(shell_value "$assignments" READELF || true)
  if [[ -n "$readelf" && -x "$readelf" ]]; then
    printf '%s\n' "$readelf"
    return 0
  fi
  return 1
}

find_target_cc() {
  local target_id=$1
  local assignments
  local cc

  assignments=$("$repo_root/scripts/discover_target_tools.sh" \
    "$repo_root/build/$target_id-release" "$target_id")
  cc=$(shell_value "$assignments" CC || true)
  if [[ -n "$cc" && -x "$cc" ]]; then
    printf '%s\n' "$cc"
    return 0
  fi
  return 1
}

build_cache_value() {
  local target_id=$1
  local key=$2
  local cache="$repo_root/build/$target_id-release/CMakeCache.txt"
  local value

  [[ -f "$cache" ]] || return 1
  value=$(sed -n "s#^${key}:[^=]*=##p" "$cache" | tail -n 1)
  if [[ "$value" == \'*\' && "$value" == *\' ]]; then
    value=${value:1:${#value}-2}
  fi
  value=${value#"${value%%[![:space:]]*}"}
  value=${value%"${value##*[![:space:]]}"}
  printf '%s\n' "$value"
}

pc_value() {
  local pc=$1
  local key=$2

  sed -n "s/^${key}=//p" "$pc" | tail -n 1
}

join_by() {
  local delimiter=$1
  shift
  local IFS=$delimiter

  printf '%s\n' "$*"
}

write_extracted_sdk_smoke_source() {
  local path=$1

  cat >"$path" <<'EOF'
#include <cai/cai.h>
#include <cai/mcp.h>
#include <cai/tools/exec.h>
#include <cai/tools/read.h>
#include <cai/tools/revgeo.h>
#include <cai/tools/searxng.h>
#include <cai/tools/todo.h>
#include <lonejson.h>

int main(void) {
  cai_client_config config;
  cai_error error;
  int (*open_fn)(const cai_client_config *, cai_client **, cai_error *);

  cai_client_config_init(&config);
  cai_error_init(&error);
  cai_error_cleanup(&error);
  open_fn = cai_client_open;
  return open_fn == 0 ? 1 : 0;
}
EOF
}

write_extracted_sdk_smoke_cmake() {
  local path=$1

  cat >"$path" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(cai_extracted_sdk_smoke LANGUAGES C)
find_package(cai CONFIG REQUIRED)
if(NOT TARGET "${CAI_SMOKE_TARGET}")
  message(FATAL_ERROR "packaged cai target not available: ${CAI_SMOKE_TARGET}")
endif()
add_executable(cai_extracted_sdk_smoke main.c)
target_link_libraries(cai_extracted_sdk_smoke PRIVATE "${CAI_SMOKE_TARGET}")
EOF
}

require_dependency_prefix() {
  local prefix=$1
  local name=$2
  local artifact=$3

  if [[ ! -d "$prefix" ]]; then
    diagnostic_fail package-verify extracted-sdk-smoke external-dependency-missing \
      "dependency prefix required to smoke-test extracted SDK is missing: $name" \
      "$artifact" \
      "run make deps-release or make release-matrix to cache $name for this target"
  fi
}

verify_extracted_sdk_cmake_variant() {
  local root_dir=$1
  local target_id=$2
  local cc=$3
  local tool_path=$4
  local smoke_target=$5
  local cmake_prefix_path=$6
  local pkg_config_path=$7
  local source_dir=$8
  local variant_name=$9
  local build_dir="$source_dir/build-$variant_name"
  local generator
  local cache_value
  local darwin_min_version_flag=
  local cmake_args=()

  command -v cmake >/dev/null 2>&1 || diagnostic_fail \
    package-verify extracted-sdk-smoke external-tool-unavailable \
    "cmake is required to smoke-test extracted SDK CMake metadata" \
    "$root_dir" \
    "install cmake and rerun make package-verify"

  cmake_args=(-S "$source_dir" -B "$build_dir")
  generator=$(build_cache_value "$target_id" CMAKE_GENERATOR || true)
  if [[ -n "$generator" ]]; then
    cmake_args+=(-G "$generator")
  fi
  cmake_args+=(
    "-DCMAKE_C_COMPILER=$cc"
    "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
    "-Dcai_DIR=$root_dir/lib/cmake/cai"
    "-DCAI_SMOKE_TARGET=$smoke_target"
    "-DCMAKE_PREFIX_PATH=$cmake_prefix_path"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH"
  )
  cache_value=$(build_cache_value "$target_id" CMAKE_SYSROOT || true)
  if [[ -n "$cache_value" ]]; then
    cmake_args+=("-DCMAKE_SYSROOT=$cache_value")
  fi
  cache_value=$(build_cache_value "$target_id" CMAKE_AR || true)
  if [[ -n "$cache_value" ]]; then
    cmake_args+=("-DCMAKE_AR=$cache_value")
  fi
  cache_value=$(build_cache_value "$target_id" CMAKE_RANLIB || true)
  if [[ -n "$cache_value" ]]; then
    cmake_args+=("-DCMAKE_RANLIB=$cache_value")
  fi
  cache_value=$(build_cache_value "$target_id" CMAKE_LINKER || true)
  if [[ -n "$cache_value" ]]; then
    cmake_args+=("-DCMAKE_LINKER=$cache_value")
  fi
  if [[ "$target_id" == *-apple-darwin ]]; then
    cmake_args+=("-DCMAKE_SYSTEM_NAME=Darwin")
    cmake_args+=("-DCMAKE_SYSTEM_PROCESSOR=arm64")
    cache_value=$(build_cache_value "$target_id" CMAKE_OSX_SYSROOT || true)
    if [[ -n "$cache_value" ]]; then
      cmake_args+=("-DCMAKE_OSX_SYSROOT=$cache_value")
    fi
    cache_value=$(build_cache_value "$target_id" CMAKE_OSX_DEPLOYMENT_TARGET || true)
    if [[ -n "$cache_value" ]]; then
      cmake_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=$cache_value")
      darwin_min_version_flag="-mmacosx-version-min=$cache_value"
    fi
    cache_value=$(build_cache_value "$target_id" CMAKE_C_FLAGS || true)
    if [[ -n "$darwin_min_version_flag" ]]; then
      cmake_args+=("-DCMAKE_C_FLAGS=${cache_value:+$cache_value }$darwin_min_version_flag")
    fi
    cache_value=$(build_cache_value "$target_id" CPKT_OTOOL || true)
    if [[ -n "$cache_value" ]]; then
      cmake_args+=("-DCPKT_OTOOL=$cache_value")
    fi
    cache_value=$(build_cache_value "$target_id" CMAKE_EXE_LINKER_FLAGS || true)
    if [[ -n "$cache_value" ]]; then
      if [[ -n "$darwin_min_version_flag" && "$cache_value" != *"$darwin_min_version_flag"* ]]; then
        cache_value="$cache_value $darwin_min_version_flag"
      fi
      cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=$cache_value")
    fi
    cache_value=$(build_cache_value "$target_id" CMAKE_SHARED_LINKER_FLAGS || true)
    if [[ -n "$cache_value" ]]; then
      if [[ -n "$darwin_min_version_flag" && "$cache_value" != *"$darwin_min_version_flag"* ]]; then
        cache_value="$cache_value $darwin_min_version_flag"
      fi
      cmake_args+=("-DCMAKE_SHARED_LINKER_FLAGS=$cache_value")
    fi
    cache_value=$(build_cache_value "$target_id" CMAKE_MODULE_LINKER_FLAGS || true)
    if [[ -n "$cache_value" ]]; then
      if [[ -n "$darwin_min_version_flag" && "$cache_value" != *"$darwin_min_version_flag"* ]]; then
        cache_value="$cache_value $darwin_min_version_flag"
      fi
      cmake_args+=("-DCMAKE_MODULE_LINKER_FLAGS=$cache_value")
    fi
  else
    cmake_args+=("-DCMAKE_SYSTEM_NAME=Linux")
    case "$target_id" in
      x86_64-*) cmake_args+=("-DCMAKE_SYSTEM_PROCESSOR=x86_64") ;;
      aarch64-*) cmake_args+=("-DCMAKE_SYSTEM_PROCESSOR=aarch64") ;;
      armhf-*) cmake_args+=("-DCMAKE_SYSTEM_PROCESSOR=arm") ;;
    esac
  fi

  if ! PATH="$tool_path" PKG_CONFIG_PATH="$pkg_config_path" cmake "${cmake_args[@]}"; then
    diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
      "failed to configure extracted SDK CMake consumer for $smoke_target" \
      "$root_dir" \
      "inspect $build_dir/CMakeFiles/CMakeConfigureLog.yaml and packaged CMake metadata"
  fi
  if ! PATH="$tool_path" PKG_CONFIG_PATH="$pkg_config_path" cmake --build "$build_dir"; then
    diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
      "failed to build extracted SDK CMake consumer for $smoke_target" \
      "$root_dir" \
      "inspect $build_dir build output and packaged CMake link interface"
  fi
}

verify_extracted_sdk_pkg_config_variant() {
  local root_dir=$1
  local target_id=$2
  local cc=$3
  local tool_path=$4
  local pkg_config_path=$5
  local source=$6
  local output=$7
  local link_mode=$8
  local cflags
  local libs
  local pc_dirs=()
  local pc_dir
  local lib_dir
  local old_ifs
  local cache_value
  local rpath_link_flags=()
  local static_link_flags=()

  command -v pkg-config >/dev/null 2>&1 || diagnostic_fail \
    package-verify extracted-sdk-smoke external-tool-unavailable \
    "pkg-config is required to smoke-test extracted SDK pkg-config metadata" \
    "$root_dir" \
    "install pkg-config and rerun make package-verify"

  cflags=$(PKG_CONFIG_PATH="$pkg_config_path" pkg-config --cflags cai) ||
    diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
      "failed to resolve extracted SDK pkg-config cflags" \
      "$root_dir" \
      "inspect $root_dir/lib/pkgconfig/cai.pc"
  if [[ "$link_mode" == "static" ]]; then
    libs=$(PKG_CONFIG_PATH="$pkg_config_path" pkg-config --static --libs cai) ||
      diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
        "failed to resolve extracted SDK static pkg-config libs" \
        "$root_dir" \
        "inspect $root_dir/lib/pkgconfig/cai.pc and dependency .pc files"
    if [[ "$target_id" == *-linux-* ]]; then
      static_link_flags=(-static)
    fi
  else
    libs=$(PKG_CONFIG_PATH="$pkg_config_path" pkg-config --libs cai) ||
      diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
        "failed to resolve extracted SDK pkg-config libs" \
        "$root_dir" \
        "inspect $root_dir/lib/pkgconfig/cai.pc and dependency .pc files"
    if [[ "$target_id" == *-linux-* ]]; then
      old_ifs=$IFS
      IFS=:
      read -r -a pc_dirs <<<"$pkg_config_path"
      IFS=$old_ifs
      for pc_dir in "${pc_dirs[@]}"; do
        lib_dir=${pc_dir%/pkgconfig}
        if [[ -d "$lib_dir" ]]; then
          rpath_link_flags+=("-Wl,-rpath-link,$lib_dir")
        fi
      done
    fi
  fi

  if [[ "$target_id" == *-apple-darwin ]]; then
    cache_value=$(build_cache_value "$target_id" CMAKE_OSX_DEPLOYMENT_TARGET || true)
    if [[ -n "$cache_value" ]]; then
      cflags+=" -mmacosx-version-min=$cache_value"
    fi
  fi

  # shellcheck disable=SC2086
  if ! PATH="$tool_path" PKG_CONFIG_PATH="$pkg_config_path" "$cc" -std=c89 -Wall -Wextra \
    -Werror "${static_link_flags[@]}" "${rpath_link_flags[@]}" \
    $cflags "$source" $libs -o "$output"; then
    diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
      "failed to build extracted SDK $link_mode pkg-config consumer" \
      "$root_dir" \
      "inspect pkg-config dependency closure and packaged link flags"
  fi
}

verify_extracted_sdk_smoke() {
  local root_dir=$1
  local target_id=$2
  local pc="$root_dir/lib/pkgconfig/cai.pc"
  local c_pkt_version
  local lonejson_version
  local pslog_version
  local c_pkt_prefix
  local lonejson_prefix
  local pslog_prefix
  local cmake_prefixes=()
  local pkg_config_dirs=()
  local include_dirs=()
  local cmake_prefix_path
  local pkg_config_path
  local cc
  local work_dir
  local cmake_dir
  local source
  local cc_dir
  local tool_path

  c_pkt_version=$(pc_value "$pc" c_pkt_systems_version)
  lonejson_version=$(pc_value "$pc" lonejson_version)
  [[ -n "$c_pkt_version" ]] || fail "cai.pc is missing c.pkt.systems version"
  [[ -n "$lonejson_version" ]] || fail "cai.pc is missing lonejson version"

  c_pkt_prefix="$repo_root/.cache/deps/c.pkt.systems-$c_pkt_version-$target_id"
  lonejson_prefix="$repo_root/.cache/deps/liblonejson-$lonejson_version-$target_id"
  pslog_version=$(sed -n \
    's/^CAI_PSLOG_VERSION[[:space:]]*[?:]*=[[:space:]]*//p' \
    "$repo_root/Makefile" | tail -n 1)
  pslog_prefix="$repo_root/.cache/deps/libpslog-$pslog_version-$target_id"

  require_dependency_prefix "$c_pkt_prefix" "c.pkt.systems" "$root_dir"
  require_dependency_prefix "$lonejson_prefix" "liblonejson" "$root_dir"

  cmake_prefixes=("$root_dir" "$c_pkt_prefix" "$lonejson_prefix")
  pkg_config_dirs=(
    "$root_dir/lib/pkgconfig"
    "$c_pkt_prefix/lib/pkgconfig"
    "$lonejson_prefix/lib/pkgconfig"
  )
  include_dirs=("-I$root_dir/include" "-I$lonejson_prefix/include")
  if [[ -n "$pslog_version" && -d "$pslog_prefix" ]]; then
    cmake_prefixes+=("$pslog_prefix")
    pkg_config_dirs+=("$pslog_prefix/lib/pkgconfig")
  fi
  if [[ -d "$c_pkt_prefix/include" ]]; then
    include_dirs+=("-I$c_pkt_prefix/include")
  fi

  cmake_prefix_path=$(join_by ';' "${cmake_prefixes[@]}")
  pkg_config_path=$(join_by ':' "${pkg_config_dirs[@]}")
  cc=$(find_target_cc "$target_id") || diagnostic_fail \
    package-verify extracted-sdk-smoke external-tool-unavailable \
    "target-correct C compiler is required to smoke-test extracted SDK" \
    "$root_dir" \
    "configure $target_id-release or set CMAKE_C_COMPILER in that build"

  tool_path=$PATH
  if [[ "$target_id" == *-apple-darwin ]]; then
    cc_dir=$(dirname "$cc")
    tool_path="$cc_dir:$PATH"
  fi

  work_dir=$(mktemp -d)
  source="$work_dir/main.c"
  write_extracted_sdk_smoke_source "$source"

  if ! PATH="$tool_path" "$cc" -std=c89 -Wall -Wextra -Werror \
    "${include_dirs[@]}" -c "$source" -o "$work_dir/direct.o"; then
    diagnostic_fail package-verify extracted-sdk-smoke package-unusable \
      "failed to compile extracted SDK public headers with target compiler" \
      "$root_dir" \
      "inspect public includes and dependency include metadata"
  fi

  cmake_dir="$work_dir/cmake-consumer"
  mkdir -p "$cmake_dir"
  cp "$source" "$cmake_dir/main.c"
  write_extracted_sdk_smoke_cmake "$cmake_dir/CMakeLists.txt"

  if compgen -G "$root_dir/lib/libcai.so*" >/dev/null ||
     compgen -G "$root_dir/lib/libcai*.dylib" >/dev/null; then
    verify_extracted_sdk_cmake_variant "$root_dir" "$target_id" \
      "$cc" "$tool_path" "cai::cai_shared" "$cmake_prefix_path" "$pkg_config_path" \
      "$cmake_dir" shared
    verify_extracted_sdk_pkg_config_variant "$root_dir" "$target_id" "$cc" "$tool_path" \
      "$pkg_config_path" "$source" "$work_dir/pkg-config-shared" shared
  fi
  if [[ -f "$root_dir/lib/libcai.a" ]]; then
    verify_extracted_sdk_cmake_variant "$root_dir" "$target_id" \
      "$cc" "$tool_path" "cai::cai_static" "$cmake_prefix_path" "$pkg_config_path" \
      "$cmake_dir" static
    verify_extracted_sdk_pkg_config_variant "$root_dir" "$target_id" "$cc" "$tool_path" \
      "$pkg_config_path" "$source" "$work_dir/pkg-config-static" static
  fi

  rm -rf "$work_dir"
}

verify_darwin_runpath() {
  local root_dir=$1
  local target_id=$2
  local dylib
  local install_name
  local dependencies
  local load_commands
  local otool

  otool=$(find_target_otool "$target_id") || diagnostic_fail \
    package-verify darwin-loader-metadata external-tool-unavailable \
    "target-correct otool is required to verify Darwin Mach-O metadata" \
    "$root_dir" \
    "configure $target_id-release or set CAI_OTOOL to a target-capable otool"
  while IFS= read -r dylib; do
    [[ -L "$dylib" ]] && continue
    install_name=$("$otool" -D "$dylib" 2>/dev/null | sed '1d' || true)
    [[ -n "$install_name" ]] || fail "could not inspect Darwin install name: $dylib"
    if ! grep -E '^@rpath/libcai\.[0-9]+\.dylib$' <<<"$install_name" >/dev/null; then
      printf '%s\n' "$install_name" >&2
      fail "Darwin shared library install name is not ABI-versioned @rpath: $dylib"
    fi
    dependencies=$("$otool" -L "$dylib" 2>/dev/null | sed '1d' || true)
    [[ -n "$dependencies" ]] || fail "could not inspect Darwin dependencies: $dylib"
    if grep -E '^[[:space:]]*/(home|Users|opt|tmp|var|lib|usr/local)/|\.cache/deps' \
      <<<"$dependencies" >/dev/null; then
      printf '%s\n' "$dependencies" >&2
      fail "Darwin shared library has non-system absolute dependency path: $dylib"
    fi
    load_commands=$("$otool" -l "$dylib" 2>/dev/null | sed '1d' || true)
    [[ -n "$load_commands" ]] || fail "could not inspect Darwin shared library: $dylib"
    if ! grep -A2 'LC_RPATH' <<<"$load_commands" | grep -F 'path @loader_path' >/dev/null; then
      printf '%s\n' "$load_commands" >&2
      fail "Darwin shared library does not use @loader_path rpath: $dylib"
    fi
    if grep -E '/home/|/Users/|/opt/|/tmp/|/var/|\.cache/deps' <<<"$load_commands" >/dev/null; then
      printf '%s\n' "$load_commands" >&2
      fail "Darwin shared library has host-specific load command: $dylib"
    fi
  done < <(find "$root_dir/lib" -maxdepth 1 -type f -name 'libcai*.dylib' -print)
}

verify_binary_archive() {
  local archive=$1
  local root=$2
  local listing=$3
  local extract_root=$4
  local root_dir="$extract_root/$root"

  require_member "$listing" "$root/include/cai/cai.h"
  require_member "$listing" "$root/include/cai/mcp.h"
  require_member "$listing" "$root/include/cai/tools/revgeo.h"
  require_member "$listing" "$root/include/cai/tools/searxng.h"
  require_member "$listing" "$root/include/cai/tools/todo.h"
  require_member "$listing" "$root/lib/pkgconfig/cai.pc"
  require_member "$listing" "$root/lib/cmake/cai/cai-config.cmake"
  require_member "$listing" "$root/share/cai/dependencies.json"
  require_member "$listing" "$root/share/doc/libcai/README.md"
  require_member "$listing" "$root/share/doc/libcai/LICENSE"
  require_member "$listing" "$root/share/doc/libcai/docs/model-metadata.md"
  require_no_member "$listing" "$root/include/lonejson.h"
  require_no_member "$listing" "$root/include/pslog.h"
  require_no_member_glob "$listing" "^$root/lib/lib(lonejson|pslog|curl)\\."
  require_no_member_glob "$listing" "^$root/lib/.*(asan|ubsan|tsan|msan)"
  verify_no_host_paths "$root_dir"
  verify_no_sanitizer_artifacts "$root_dir"
  verify_dependency_manifest "$root_dir" "${root#cai-$version-}"
  verify_extracted_sdk_smoke "$root_dir" "${root#cai-$version-}"
  if [[ "$root" == *-linux-* ]]; then
    verify_linux_runpath "$root_dir" "${root#cai-$version-}"
  elif [[ "$root" == *-apple-darwin ]]; then
    verify_darwin_runpath "$root_dir" "${root#cai-$version-}"
  fi
}

verify_source_archive() {
  local root=$1
  local listing=$2

  require_member "$listing" "$root/CMakeLists.txt"
  require_member "$listing" "$root/README.md"
  require_member "$listing" "$root/docs/model-metadata.md"
  require_member "$listing" "$root/LICENSE"
  require_member "$listing" "$root/VERSION"
  require_member "$listing" "$root/RELEASE_MANIFEST"
  require_no_member "$listing" "$root/.git/config"
  require_no_member "$listing" "$root/.env"
  verify_source_matches_git_manifest "$root" "$listing"
}

verify_lua_source_archive() {
  local root=$1
  local listing=$2
  local extract_root=$3

  require_member "$listing" "$root/LICENSE"
  require_member "$listing" "$root/README.md"
  require_member "$listing" "$root/docs/model-metadata.md"
  require_member "$listing" "$root/VERSION"
  require_member "$listing" "$root/RELEASE_MANIFEST"
  require_member "$listing" "$root/cai.rockspec.in"
  require_member "$listing" "$root/scripts/build_lua_rock.sh"
  require_member "$listing" "$root/scripts/render_release_rockspec.sh"
  require_member "$listing" "$root/lua/cai_lua.c"
  require_member "$listing" "$root/include/cai/cai.h"
  require_member "$listing" "$root/include/cai/mcp.h"
  require_member "$listing" "$root/include/cai/models.h"
  require_no_member "$listing" "$root/.git/config"
  require_no_member "$listing" "$root/.env"
  verify_no_private_text "$extract_root/$root"
}

verify_rockspec_file() {
  local rockspec=$1
  local matches

  require_file "$rockspec"
  matches=$(grep -n -E '/home/|/Users/|/opt/|\.cache/deps|\.\./' \
    "$rockspec" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "rockspec contains host-specific or out-of-repository paths: $rockspec"
  fi
}

verify_src_rock() {
  local rock=$1
  local listing
  local extract_root
  local rockspec_name="cai-$version-1.rockspec"
  local source_name="cai-lua-$version.tar.gz"
  local source_listing
  local source_extract_root

  command -v unzip >/dev/null 2>&1 || fail "unzip is required to verify source rock"
  require_file "$rock"
  listing=$(mktemp)
  extract_root=$(mktemp -d)
  trap 'rm -f "$listing"; rm -rf "$extract_root"' RETURN

  unzip -Z1 "$rock" >"$listing"
  require_member "$listing" "$rockspec_name"
  require_member "$listing" "$source_name"
  unzip -q "$rock" -d "$extract_root"
  verify_rockspec_file "$extract_root/$rockspec_name"

  source_listing=$(mktemp)
  source_extract_root=$(mktemp -d)
  tar -tzf "$extract_root/$source_name" >"$source_listing"
  verify_single_root "$extract_root/$source_name" "cai-$version" "$source_listing"
  tar -xzf "$extract_root/$source_name" -C "$source_extract_root"
  verify_lua_source_archive "cai-$version" "$source_listing" "$source_extract_root"
  rm -f "$source_listing"
  rm -rf "$source_extract_root"

  rm -f "$listing"
  rm -rf "$extract_root"
  trap - RETURN
}

verify_archive() {
  local archive=$1
  local name
  local root
  local expected_root
  local listing
  local extract_root

  name=$(basename "$archive")
  root=${name%.tar.gz}
  expected_root=$root
  if [[ "$root" == "cai-lua-$version" ]]; then
    expected_root="cai-$version"
  fi
  listing=$(mktemp)
  extract_root=$(mktemp -d)
  trap 'rm -f "$listing"; rm -rf "$extract_root"' RETURN

  tar -tzf "$archive" >"$listing"
  verify_listing_has_no_host_paths "$listing"
  verify_single_root "$archive" "$expected_root" "$listing"
  tar -xzf "$archive" -C "$extract_root"

  if [[ "$root" == "cai-$version" ]]; then
    verify_source_archive "$root" "$listing"
  elif [[ "$root" == "cai-lua-$version" ]]; then
    verify_lua_source_archive "$expected_root" "$listing" "$extract_root"
  else
    verify_binary_archive "$archive" "$root" "$listing" "$extract_root"
  fi
  verify_no_private_bytes "$extract_root"

  rm -f "$listing"
  rm -rf "$extract_root"
  trap - RETURN
}

if [[ "$self_test" == "1" ]]; then
  return 0 2>/dev/null || exit 0
fi

verify_checksum_file

mapfile -t artifacts < <(read_checksum_artifacts)
if [[ ${#artifacts[@]} -eq 0 ]]; then
  fail "checksum manifest lists no release artifacts"
fi
verify_manifest_artifacts "${artifacts[@]}"
verify_dist_manifest_closure "${artifacts[@]}"

archive_count=0
for artifact in "${artifacts[@]}"; do
  case "$artifact" in
    *.tar.gz)
      verify_archive "$dist_dir/$artifact"
      archive_count=$((archive_count + 1))
      ;;
    *.rockspec)
      verify_rockspec_file "$dist_dir/$artifact"
      ;;
    *.src.rock)
      verify_src_rock "$dist_dir/$artifact"
      ;;
    *)
      fail "checksum manifest lists unsupported release artifact: $artifact"
      ;;
  esac
done

printf 'Verified %d cai release archive(s) for version %s\n' \
  "$archive_count" "$version"
