#!/usr/bin/env bash
set -eu

if [ "$#" -ne 6 ]; then
  printf 'usage: %s CC CFLAGS LIBFLAG OBJ_EXTENSION LIB_EXTENSION LUA_INCDIR\n' "$0" >&2
  exit 1
fi

cc="$1"
cflags="$2"
libflag="$3"
obj_ext="$4"
lib_ext="$5"
lua_incdir="$6"

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

linkflags="${LDFLAGS:-}"
if [ "$(uname -s)" = "Linux" ]; then
  linkflags="${linkflags} -Wl,--allow-shlib-undefined"
fi

common_cflags="${cflags} -I${lua_incdir} ${cai_cflags}"
run_cc ${common_cflags} -c "${repo_root}/lua/cai_lua.c" -o "${object_path}"
run_cc ${libflag} -o "${module_path}" "${object_path}" ${linkflags} ${cai_libs}
