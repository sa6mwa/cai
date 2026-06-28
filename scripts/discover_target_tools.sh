#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'usage: %s <build-dir> <target-id>\n' "$0" >&2
  exit 2
fi

build_dir=$1
target_id=$2
cache="$build_dir/CMakeCache.txt"

cache_value() {
  local key=$1
  [[ -f "$cache" ]] || return 1
  sed -n "s#^${key}:[^=]*=##p" "$cache" | tail -n 1
}

is_exe() {
  [[ -n "${1:-}" && -x "$1" ]]
}

tool_dir_from_cc() {
  local cc=$1
  [[ "$cc" == */* ]] || return 1
  dirname "$cc"
}

host_from_cc() {
  local cc=$1
  local base
  base=$(basename "$cc")
  case "$base" in
    *-clang | *-clang++ | *-cc | *-gcc | *-g++)
      printf '%s\n' "${base%-clang}" | sed 's/-clang++$//;s/-cc$//;s/-gcc$//;s/-g++$//'
      ;;
  esac
}

path_tool() {
  local name
  for name in "$@"; do
    if command -v "$name" >/dev/null 2>&1; then
      command -v "$name"
      return 0
    fi
  done
  return 1
}

discover_tool() {
  local output_key=$1
  local override_var=$2
  local cache_keys=$3
  local sibling_names=$4
  local path_names=$5
  local override_value=${!override_var:-}
  local cache_key cache_result cc cc_dir cc_host candidate name

  if is_exe "$override_value"; then
    printf '%s\n' "$override_value"
    return 0
  fi

  IFS=',' read -r -a cache_key_array <<<"$cache_keys"
  for cache_key in "${cache_key_array[@]}"; do
    cache_result=$(cache_value "$cache_key" || true)
    if is_exe "$cache_result"; then
      printf '%s\n' "$cache_result"
      return 0
    fi
  done

  cc=$(cache_value CMAKE_C_COMPILER || true)
  if [[ -n "$cc" ]]; then
    cc_dir=$(tool_dir_from_cc "$cc" || true)
    cc_host=$(host_from_cc "$cc" || true)
    if [[ -n "$cc_dir" && -n "$cc_host" ]]; then
      IFS=',' read -r -a sibling_name_array <<<"$sibling_names"
      for name in "${sibling_name_array[@]}"; do
        candidate="$cc_dir/$cc_host-$name"
        if is_exe "$candidate"; then
          printf '%s\n' "$candidate"
          return 0
        fi
      done
    fi
    if [[ -n "$cc_dir" ]]; then
      IFS=',' read -r -a sibling_name_array <<<"$sibling_names"
      for name in "${sibling_name_array[@]}"; do
        candidate="$cc_dir/$name"
        if is_exe "$candidate"; then
          printf '%s\n' "$candidate"
          return 0
        fi
      done
    fi
  fi

  if [[ "$target_id" == *-apple-darwin ]]; then
    local osxcross_root osxcross_host osxcross_bin
    osxcross_root=${OSXCROSS_ROOT:-${HOME:-}/.local/cross/osxcross}
    osxcross_host=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
    osxcross_bin="$osxcross_root/bin"
    IFS=',' read -r -a sibling_name_array <<<"$sibling_names"
    for name in "${sibling_name_array[@]}"; do
      candidate="$osxcross_bin/$osxcross_host-$name"
      if is_exe "$candidate"; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  fi

  IFS=',' read -r -a path_name_array <<<"$path_names"
  path_tool "${path_name_array[@]}" || return 1
}

cc=$(cache_value CMAKE_C_COMPILER || true)
cc_host=""
if [[ -n "$cc" ]]; then
  cc_host=$(host_from_cc "$cc" || true)
fi

strip_tool=$(discover_tool STRIP CAI_STRIP "CMAKE_STRIP" "strip" "strip" || true)
install_name_tool=$(discover_tool INSTALL_NAME_TOOL CAI_INSTALL_NAME_TOOL "CMAKE_INSTALL_NAME_TOOL" "install_name_tool" "install_name_tool" || true)
otool_tool=$(discover_tool OTOOL CAI_OTOOL "CPKT_OTOOL,CMAKE_OTOOL" "otool" "llvm-otool-20,llvm-otool,otool" || true)
readelf_tool=$(discover_tool READELF CAI_READELF "CMAKE_READELF" "readelf" "readelf" || true)

printf 'CC=%q\n' "$cc"
printf 'TARGET_HOST=%q\n' "$cc_host"
printf 'STRIP=%q\n' "$strip_tool"
printf 'INSTALL_NAME_TOOL=%q\n' "$install_name_tool"
printf 'OTOOL=%q\n' "$otool_tool"
printf 'READELF=%q\n' "$readelf_tool"
