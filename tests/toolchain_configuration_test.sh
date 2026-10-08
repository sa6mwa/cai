#!/usr/bin/env bash
# Native fixtures prove read-only toolchain loading; no compiler or downloader runs.
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/toolchain-configuration.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "toolchain configuration regression failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; cat "$scratch/err" >&2 2>/dev/null || true' ERR
mkdir -p "$scratch/scripts" "$scratch/cmake/toolchains" "$scratch/base with spaces/fixture-bootlin/bin" "$scratch/sysroot/lib"
cp "$repo/cmake/toolchains/"{linux-bootlin,linux-aflpp}.cmake "$scratch/cmake/toolchains/"
cp "$repo/scripts/cpkt-aflpp.sh" "$scratch/scripts/"
export CAI_CONFIG_ROOT="$scratch/base with spaces/fixture-bootlin" CAI_CONFIG_SYSROOT="$scratch/sysroot" CAI_CONFIG_LOG="$scratch/events"
cat >"$scratch/scripts/cpkt-toolchains.sh" <<'SH'
#!/usr/bin/env bash
printf 'bootlin %s\n' "$*" >>"$CAI_CONFIG_LOG"
[[ "$1" == discover && ${CAI_CONFIG_DISCOVERY_FAIL:-0} != 1 ]] || exit 7
printf 'status=%s\ncache=/fixture/status=ready\nroot=%s\nsysroot=%s\ntarget_triple=x86_64-buildroot-linux-gnu\n' "${CAI_CONFIG_STATUS:-ready}" "$CAI_CONFIG_ROOT" "$CAI_CONFIG_SYSROOT"
for key in cc cxx ld ar ranlib strip nm objcopy objdump addr2line gdb readelf; do
  printf '%s=%s/bin/compiler\n' "$key" "$CAI_CONFIG_ROOT"
done
SH
cat >"$CAI_CONFIG_ROOT/bin/compiler" <<'SH'
#!/usr/bin/env bash
case "$1" in
  -print-prog-name=ld) printf '%s/bin/compiler\n' "$CAI_CONFIG_ROOT" ;;
  -print-file-name=libc.so) printf '%s/lib/libc.so\n' "$CAI_CONFIG_SYSROOT" ;;
  *) exit 7 ;;
esac
SH
chmod +x "$scratch/scripts/cpkt-toolchains.sh" "$CAI_CONFIG_ROOT/bin/compiler"
touch "$CAI_CONFIG_SYSROOT/lib/libc.so"
load_bootlin() { cmake -DCPKT_TARGET_ID=x86_64-linux-gnu -P "$scratch/cmake/toolchains/linux-bootlin.cmake" >"$scratch/out" 2>"$scratch/err"; }
: >"$CAI_CONFIG_LOG"
load_bootlin
[[ $(cat "$CAI_CONFIG_LOG") == 'bootlin discover x86_64-linux-gnu' ]]
if CAI_CONFIG_STATUS=missing load_bootlin; then exit 1; fi
grep -q 'Missing pinned x86_64-linux-gnu collection' "$scratch/err"
if CAI_CONFIG_DISCOVERY_FAIL=1 load_bootlin; then exit 1; fi
grep -q 'failed to inspect pinned' "$scratch/err"
# Cold AFL discovery/env must neither acquire Bootlin nor create the tool cache.
export CPKT_TOOLCHAIN_CACHE="$scratch/tool-cache"
for command in discover env; do
  : >"$CAI_CONFIG_LOG"
  if bash "$scratch/scripts/cpkt-aflpp.sh" "$command" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'Missing AFL++ collection' "$scratch/err"
  [[ ! -s "$scratch/out" && ! -e "$CPKT_TOOLCHAIN_CACHE" ]]
  [[ $(cat "$CAI_CONFIG_LOG") == 'bootlin discover x86_64-linux-gnu' ]]
done
# A prepared synthetic collection supplies report/export fields without builds.
afl_root="$CPKT_TOOLCHAIN_CACHE/roots/aflplusplus-5.02c-x86_64-linux-gnu-fixture-bootlin"
mkdir -p "$afl_root/bin" "$afl_root/lib/afl"
for tool in afl-fuzz afl-showmap cpkt-afl-gcc cpkt-afl-g++; do
  cp "$CAI_CONFIG_ROOT/bin/compiler" "$afl_root/bin/$tool"
done
touch "$afl_root/lib/afl/afl-gcc-pass.so" "$afl_root/lib/afl/afl-compiler-rt.o" "$afl_root/.cpkt-aflpp-revision-2-fixture-bootlin"
for command in discover env; do
  : >"$CAI_CONFIG_LOG"
  bash "$scratch/scripts/cpkt-aflpp.sh" "$command" >"$scratch/out" 2>"$scratch/err"
  grep -Fq "$afl_root" "$scratch/out"
  [[ $(cat "$CAI_CONFIG_LOG") == 'bootlin discover x86_64-linux-gnu' ]]
done
cat >"$scratch/load-afl.cmake" <<'CMAKE'
include("${CMAKE_CURRENT_LIST_DIR}/cmake/toolchains/linux-aflpp.cmake")
file(WRITE "${CMAKE_CURRENT_LIST_DIR}/chosen-compiler" "${CMAKE_C_COMPILER}")
CMAKE
cmake -P "$scratch/load-afl.cmake" >"$scratch/out" 2>"$scratch/err"
[[ $(cat "$scratch/chosen-compiler") == "$afl_root/bin/cpkt-afl-gcc" ]]
! grep -q ensure "$CAI_CONFIG_LOG"
# Source reconstruction owns preparation before its configure/build/test commands.
owner="$scratch/source-smoke"
mkdir -p "$owner/scripts" "$owner/bin" "$owner/package/cai-1.2.3"
printf 'fixture\n' >"$owner/CMakeLists.txt"
printf '1.2.3\n' >"$owner/package/cai-1.2.3/VERSION"
printf 'VERSION\n' >"$owner/package/cai-1.2.3/RELEASE_MANIFEST"
cp "$repo/scripts/test_release_from_source.sh" "$owner/scripts/"
cat >"$owner/scripts/cpkt-toolchains.sh" <<'SH'
#!/usr/bin/env bash
printf 'prepare %s\n' "$*" >>"$CAI_CONFIG_LOG"
[[ "$*" == 'ensure x86_64-linux-gnu' && ${CAI_CONFIG_PREP_FAIL:-0} != 1 ]] || exit 7
SH
cat >"$owner/bin/cmake" <<'SH'
#!/usr/bin/env bash
if [[ "$1" == --build ]]; then
  printf 'build\n' >>"$CAI_CONFIG_LOG"
else
  printf 'configure\n' >>"$CAI_CONFIG_LOG"
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == -B ]]; then
      build=$2
      mkdir -p "$build/generated/include/cai"
      printf '#define CAI_VERSION_STRING "1.2.3"\n' >"$build/generated/include/cai/version.h"
      printf 'Version: 1.2.3\n' >"$build/cai.pc"
      break
    fi
    shift
  done
fi
SH
cat >"$owner/bin/ctest" <<'SH'
#!/usr/bin/env bash
printf 'test\n' >>"$CAI_CONFIG_LOG"
SH
chmod +x "$owner/scripts/cpkt-toolchains.sh" "$owner/bin/"*
tar -czf "$owner/source.tar.gz" -C "$owner/package" cai-1.2.3
: >"$CAI_CONFIG_LOG"
PATH="$owner/bin:$PATH" bash "$owner/scripts/test_release_from_source.sh" "$owner" "$owner/source.tar.gz"
[[ $(cat "$CAI_CONFIG_LOG") == $'prepare ensure x86_64-linux-gnu\nconfigure\nbuild\ntest' ]]
: >"$CAI_CONFIG_LOG"
if CAI_CONFIG_PREP_FAIL=1 PATH="$owner/bin:$PATH" bash "$owner/scripts/test_release_from_source.sh" "$owner" "$owner/source.tar.gz" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ $(cat "$CAI_CONFIG_LOG") == 'prepare ensure x86_64-linux-gnu' ]]
