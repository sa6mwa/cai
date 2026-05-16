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

  matches=$(grep -R -I -n -E '/home/|/Users/|/opt/|/tmp/|/var/tmp|\.cache/deps' \
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
      grep -E '/home/|/Users/|/opt/|/tmp/|/var/tmp|\.cache/deps' \
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

verify_archive() {
  local archive=$1
  local name
  local root
  local listing
  local extract_root

  name=$(basename "$archive")
  root=${name%.tar.gz}
  listing=$(mktemp)
  extract_root=$(mktemp -d)
  trap 'rm -f "$listing"; rm -rf "$extract_root"' RETURN

  tar -tzf "$archive" >"$listing"
  verify_single_root "$archive" "$root" "$listing"
  tar -xzf "$archive" -C "$extract_root"

  if [[ "$root" == "cai-$version" ]]; then
    verify_source_archive "$root" "$listing"
  else
    verify_binary_archive "$archive" "$root" "$listing" "$extract_root"
  fi

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

printf 'Verified %d cai release archive(s) for version %s\n' \
  "${#archives[@]}" "$version"
