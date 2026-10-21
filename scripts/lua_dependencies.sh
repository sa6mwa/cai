#!/usr/bin/env bash
# Install a declared facade prerequisite using the configured native collection.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# == 2 ]] || exit 2
mapfile -t paths <"$root/build/debug/lua-dependency-paths.txt"
[[ ${#paths[@]} == 5 && -x ${paths[0]} ]] || { printf 'Configure the native debug prerequisites first.\n' >&2; exit 2; }
name=$1; url=$2
prefix=${paths[2]}
[[ "$name" != pslog ]] || prefix=${paths[3]}
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
for path in "${paths[@]:1:3}"; do if [[ -n "$path" ]]; then PKG_CONFIG_PATH="$path/lib/pkgconfig:$PKG_CONFIG_PATH"; fi; done
if [[ -n "$prefix" ]]; then
  export CFLAGS="${CFLAGS:-} -fPIC -I$prefix/include" LDFLAGS="${LDFLAGS:-} -L$prefix/lib"
fi
cmake "-DNAME=$name" "-DURL=$url" "-DCPKT_DEPENDENCY_CACHE=${paths[4]}" "-DOUTPUT=$root/build/debug/lua-$name-archive.txt" -P "$root/cmake/acquire_lua_rock.cmake"
read -r archive <"$root/build/debug/lua-$name-archive.txt"
args=(install --tree "$root/build/luarocks" "$archive" "CC=${paths[0]}")
[[ -z ${paths[1]} ]] || args+=("LUA_INCDIR=${paths[1]}/include")
[[ "$name" != pslog ]] || args+=("LIBPSLOG_DIR=$prefix")
mkdir -p "$root/build/luarocks"
luarocks "${args[@]}"
