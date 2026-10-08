#!/usr/bin/env bash
# LuaRocks supplies arguments; native CMake owns compilation and invalidation.
set -euo pipefail
[[ $# == 7 ]] || { printf 'usage: %s CC CFLAGS LIBFLAG OBJ_EXTENSION LIB_EXTENSION LUA_INCDIR ROCKS_TREE\n' "$0" >&2; exit 2; }
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cc=$1; flags=$2; lua_include=$6; rocks=$7
pslog_include=${PSLOG_LUA_INCLUDE_DIR:-}
for prefix in "${CAI_PREFIX:-}" "${LIBPSLOG_DIR:-}"; do
  if [[ -n "$prefix" ]]; then
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  fi
done
if [[ -z "$pslog_include" && ${rocks##*/} == rocks-* ]]; then
  prefix=${rocks%/lib/luarocks/*}; version=${rocks##*/rocks-}
  pslog_include="$prefix/share/lua/$version"
fi
[[ -n "$pslog_include" ]] || pslog_include=$(luarocks config deploy_lua_dir)
cmake -S "$root/lua" -B "$root/build/luarocks-build" -G Ninja \
  "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_MODULE_LINKER_FLAGS=${LDFLAGS:-}" \
  "-DCAI_LUA_INCLUDE_DIR=$lua_include" "-DCAI_PSLOG_LUA_INCLUDE_DIR=$pslog_include" "-DCAI_LUA_EXTENSION=$5"
cmake --build "$root/build/luarocks-build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
