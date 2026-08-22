#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 /path/to/repository /path/to/lua /path/to/e2e.lua" >&2
  exit 2
fi

root=$1
lua_bin=$2
script=$3
rock_tree=$root/build/luarocks

if [ ! -f "$rock_tree/lib/lua/5.5/cai.so" ] &&
   [ ! -f "$rock_tree/lib/lua/5.5/cai.dylib" ]; then
  echo "SKIP: build the local LuaRock before running this Lua e2e" >&2
  exit 77
fi

eval "$(luarocks path --tree "$rock_tree")"
if [ -n "${CAI_LUA_LD_LIBRARY_PATH:-}" ]; then
  LD_LIBRARY_PATH="$CAI_LUA_LD_LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  DYLD_LIBRARY_PATH="$CAI_LUA_LD_LIBRARY_PATH${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
  export DYLD_LIBRARY_PATH
fi
exec "$lua_bin" "$script"
