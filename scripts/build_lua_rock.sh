#!/usr/bin/env bash
set -eu

if [ "$#" -ne 7 ]; then
  printf 'usage: %s CC CFLAGS LIBFLAG OBJ_EXTENSION LIB_EXTENSION LUA_INCDIR ROCKS_TREE\n' "$0" >&2
  exit 1
fi

cc="$1"
cflags="$2"
libflag="$3"
obj_ext="$4"
lib_ext="$5"
lua_incdir="$6"
rocks_tree="$7"

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build_root="${repo_root}/.luarocks-build"
object_path="${build_root}/cai_lua.${obj_ext}"
module_path="${build_root}/cai.${lib_ext}"

if [ -z "${cc}" ]; then
  printf 'compiler command is empty\n' >&2
  exit 1
fi

run_cc() {
  if [ -x "${cc}" ]; then
    "${cc}" "$@"
    return "$?"
  fi
  CC_CAI="${cc}" sh -c '
    eval "set -- ${CC_CAI} \"\$@\""
    exec "$@"
  ' sh "$@"
}

mkdir -p "${build_root}"
rm -f "${object_path}" "${module_path}"

cai_cflags=""
cai_libs=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists cai; then
  cai_cflags="$(pkg-config --cflags cai)"
  cai_libs="$(pkg-config --libs cai)"
elif [ -n "${CAI_PREFIX:-}" ]; then
  cai_cflags="-I${CAI_PREFIX}/include"
  cai_libs="-L${CAI_PREFIX}/lib -lcai"
else
  printf '%s\n' 'Could not find installed cai via pkg-config. Set CAI_PREFIX or install cai first.' >&2
  exit 1
fi

pslog_cflags=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists pslog; then
  pslog_cflags="$(pkg-config --cflags pslog)"
elif [ -n "${LIBPSLOG_DIR:-}" ]; then
  pslog_cflags="-I${LIBPSLOG_DIR}/include"
else
  printf '%s\n' 'Could not find installed pslog via pkg-config. Set LIBPSLOG_DIR or install pslog first.' >&2
  exit 1
fi

pslog_lua_include_dir="${PSLOG_LUA_INCLUDE_DIR:-}"
if [ -z "${pslog_lua_include_dir}" ] && [ -n "${rocks_tree}" ]; then
  rocks_base=$(basename "${rocks_tree}")
  lua_version=${rocks_base#rocks-}
  if [ "${lua_version}" != "${rocks_base}" ]; then
    rocks_prefix=${rocks_tree%/lib/luarocks/${rocks_base}}
    if [ -f "${rocks_prefix}/share/lua/${lua_version}/pslog_lua.h" ]; then
      pslog_lua_include_dir="${rocks_prefix}/share/lua/${lua_version}"
    fi
  fi
fi
if [ -z "${pslog_lua_include_dir}" ] && command -v luarocks >/dev/null 2>&1; then
  pslog_lua_include_dir="$(luarocks config deploy_lua_dir 2>/dev/null || true)"
fi
if [ -z "${pslog_lua_include_dir}" ] || \
   [ ! -f "${pslog_lua_include_dir}/pslog_lua.h" ]; then
  printf '%s\n' \
    'Could not find pslog_lua.h from lua-pslog. Set PSLOG_LUA_INCLUDE_DIR or install lua-pslog in the active LuaRocks tree.' >&2
  exit 1
fi

linkflags="${LDFLAGS:-}"
if [ "$(uname -s)" = "Linux" ]; then
  linkflags="${linkflags} -Wl,--allow-shlib-undefined -ldl"
fi

common_cflags="${cflags} -I${lua_incdir} -I${pslog_lua_include_dir} ${cai_cflags} ${pslog_cflags}"
run_cc ${common_cflags} -c "${repo_root}/lua/cai_lua.c" -o "${object_path}"
run_cc ${libflag} -o "${module_path}" "${object_path}" ${linkflags} ${cai_libs}
