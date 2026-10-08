#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
mapfile -t paths <build/debug/lua-dependency-paths.txt
[[ ${#paths[@]} == 5 && -x ${paths[0]} ]] || { printf 'Configure the native debug producer first.\n' >&2; exit 2; }
export PKG_CONFIG_PATH="$root/build/luarocks/cai-prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
for path in "${paths[@]:1:3}"; do if [[ -n "$path" ]]; then PKG_CONFIG_PATH="$path/lib/pkgconfig:$PKG_CONFIG_PATH"; fi; done
export PSLOG_LUA_INCLUDE_DIR="$root/build/luarocks/share/lua/5.5"
args=(make --tree "$root/build/luarocks" "$1" "CC=${paths[0]}" "CFLAGS=${CFLAGS:-} ${LUA_ROCK_EXTRA_CFLAGS:--O3 -DNDEBUG}")
[[ -z ${paths[1]} ]] || args+=("LUA_INCDIR=${paths[1]}/include")
luarocks "${args[@]}"
