#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  printf 'usage: %s <repo-root> [version]\n' "$0" >&2
  exit 1
fi

repo_root=$1
version=${2:-}

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
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$dist_dir" && sha256sum -c "$(basename "$checksums")" >/dev/null)
  elif command -v shasum >/dev/null 2>&1; then
    (cd "$dist_dir" && shasum -a 256 -c "$(basename "$checksums")" >/dev/null)
  else
    fail "neither sha256sum nor shasum is available"
  fi
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

  matches=$(grep -R -I -n -E 'fsanitize|__asan|__ubsan|libasan|libubsan' \
    "$root_dir/lib" 2>/dev/null || true)
  if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches" >&2
    fail "release library contains sanitizer artifact"
  fi
}

verify_linux_runpath() {
  local root_dir=$1
  local so
  local dynamic

  command -v readelf >/dev/null 2>&1 || return 0
  while IFS= read -r so; do
    [[ -L "$so" ]] && continue
    dynamic=$(readelf -d "$so" 2>/dev/null || true)
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
    if grep -E 'NEEDED' <<<"$dynamic" | grep -E 'libasan|libubsan' \
      >/dev/null; then
      printf '%s\n' "$dynamic" >&2
      fail "shared library links sanitizer runtime: $so"
    fi
  done < <(find "$root_dir/lib" -maxdepth 1 -type f -name 'libcai.so*' -print)
}

find_otool() {
  local tool

  for tool in llvm-otool-20 llvm-otool otool; do
    if command -v "$tool" >/dev/null 2>&1; then
      printf '%s\n' "$tool"
      return 0
    fi
  done
  return 1
}

verify_darwin_runpath() {
  local root_dir=$1
  local dylib
  local load_commands
  local otool

  otool=$(find_otool) || fail "llvm-otool or otool is required to verify Darwin runpaths"
  while IFS= read -r dylib; do
    [[ -L "$dylib" ]] && continue
    load_commands=$("$otool" -l "$dylib" 2>/dev/null || true)
    [[ -n "$load_commands" ]] || fail "could not inspect Darwin shared library: $dylib"
    if ! grep -A2 'LC_RPATH' <<<"$load_commands" | grep -F 'path $ORIGIN' >/dev/null; then
      printf '%s\n' "$load_commands" >&2
      fail "Darwin shared library does not use \$ORIGIN rpath: $dylib"
    fi
    if grep -E '/home/|/Users/|/opt/|\.cache/deps' <<<"$load_commands" >/dev/null; then
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
  require_member "$listing" "$root/share/doc/libcai/README.md"
  require_member "$listing" "$root/share/doc/libcai/LICENSE"
  require_no_member "$listing" "$root/include/lonejson.h"
  require_no_member "$listing" "$root/include/pslog.h"
  require_no_member_glob "$listing" "^$root/lib/lib(lonejson|pslog|curl)\\."
  require_no_member_glob "$listing" "^$root/lib/.*(asan|ubsan)"
  verify_no_host_paths "$root_dir"
  verify_no_sanitizer_artifacts "$root_dir"
  if [[ "$root" == *-linux-* ]]; then
    verify_linux_runpath "$root_dir"
  elif [[ "$root" == *-apple-darwin ]]; then
    verify_darwin_runpath "$root_dir"
  fi
}

verify_source_archive() {
  local root=$1
  local listing=$2

  require_member "$listing" "$root/CMakeLists.txt"
  require_member "$listing" "$root/README.md"
  require_member "$listing" "$root/LICENSE"
  require_member "$listing" "$root/VERSION"
  require_member "$listing" "$root/RELEASE_MANIFEST"
  require_no_member "$listing" "$root/.git/config"
  require_no_member "$listing" "$root/.env"
}

verify_lua_source_archive() {
  local root=$1
  local listing=$2
  local extract_root=$3

  require_member "$listing" "$root/LICENSE"
  require_member "$listing" "$root/README.md"
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

verify_checksum_file

shopt -s nullglob
archives=("$dist_dir/cai-$version".tar.gz "$dist_dir/cai-$version"-*.tar.gz)
if [[ ${#archives[@]} -eq 0 ]]; then
  fail "no release archives found in $dist_dir"
fi

for archive in "${archives[@]}"; do
  verify_archive "$archive"
done

verify_rockspec_file "$dist_dir/cai-$version-1.rockspec"
verify_src_rock "$dist_dir/cai-$version-1.src.rock"

printf 'Verified %d cai release archive(s) for version %s\n' \
  "${#archives[@]}" "$version"
