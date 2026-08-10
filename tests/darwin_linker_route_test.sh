#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

host=arm64-apple-darwin25
osx_bin=$tmpdir/osxcross/bin
host_bin=$tmpdir/host/bin
log=$tmpdir/linker.log
mkdir -p "$osx_bin" "$host_bin"

cat >"$host_bin/ld" <<'EOF_HOST_LD'
#!/usr/bin/env sh
printf 'host:%s\n' "$0" >>"$CAI_LINKER_ROUTE_LOG"
exit 0
EOF_HOST_LD
chmod +x "$host_bin/ld"

cat >"$osx_bin/$host-ld" <<'EOF_TARGET_LD'
#!/usr/bin/env sh
printf 'target:%s\n' "$0" >>"$CAI_LINKER_ROUTE_LOG"
exit 0
EOF_TARGET_LD
chmod +x "$osx_bin/$host-ld"

for tool in ar ranlib otool clang++ nm; do
  printf '#!/usr/bin/env sh\nexit 0\n' >"$osx_bin/$host-$tool"
  chmod +x "$osx_bin/$host-$tool"
done

cat >"$osx_bin/$host-clang" <<'EOF_CLANG'
#!/usr/bin/env bash
set -euo pipefail
linker=
for arg in "$@"; do
  case "$arg" in
    --ld-path=*)
      linker=${arg#--ld-path=}
      ;;
  esac
done
if [[ -z "$linker" ]]; then
  linker=$(command -v ld)
fi
"$linker"
EOF_CLANG
chmod +x "$osx_bin/$host-clang"

CAI_LINKER_ROUTE_LOG=$log PATH="$host_bin:$osx_bin:$PATH" \
  "$osx_bin/$host-clang" -o "$tmpdir/unfixed" "$tmpdir/main.o"
if ! grep -q '^host:' "$log"; then
  printf 'expected unfixed route to select host ld with osxcross bin after host PATH\n' >&2
  cat "$log" >&2
  exit 1
fi

: >"$log"
CAI_LINKER_ROUTE_LOG=$log PATH="$osx_bin:$host_bin:$PATH" \
  "$osx_bin/$host-clang" "--ld-path=$osx_bin/$host-ld" \
  -o "$tmpdir/fixed" "$tmpdir/main.o"
if ! grep -q "^target:$osx_bin/$host-ld$" "$log"; then
  printf 'expected lifecycle route to select target osxcross ld\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! grep -F 'set(ENV{PATH} "${CAI_OSXCROSS_BIN_DIR}:$ENV{PATH}")' \
  "$repo_root/cmake/toolchains/arm64-apple-darwin.cmake" >/dev/null; then
  printf 'Darwin toolchain must prepend osxcross bin to PATH\n' >&2
  exit 1
fi

if ! grep -F 'set(_cai_darwin_linker_flag "--ld-path=${CMAKE_LINKER}")' \
  "$repo_root/cmake/toolchains/arm64-apple-darwin.cmake" >/dev/null; then
  printf 'Darwin toolchain must inject absolute --ld-path=${CMAKE_LINKER}\n' >&2
  exit 1
fi

if OSXCROSS_ROOT="$tmpdir/osxcross" CPKT_OSXCROSS_HOST=x86_64-apple-darwin25 \
  cmake -P "$repo_root/cmake/toolchains/arm64-apple-darwin.cmake" \
  >"$tmpdir/mismatched-host.out" 2>"$tmpdir/mismatched-host.err"; then
  printf 'expected arm64 Darwin toolchain to reject x86_64 osxcross host\n' >&2
  exit 1
fi
if ! grep -F 'requires an arm64 osxcross host triple' \
  "$tmpdir/mismatched-host.err" >/dev/null; then
  printf 'expected mismatched osxcross host diagnostic, got:\n' >&2
  cat "$tmpdir/mismatched-host.err" >&2
  exit 1
fi

if OSXCROSS_ROOT="$tmpdir/osxcross" CPKT_OSXCROSS_HOST="$host" \
  "$repo_root/scripts/osxcross_available.sh" --quiet; then
  printf 'expected incomplete osxcross SDK to be unavailable\n' >&2
  exit 1
fi

mkdir -p "$tmpdir/osxcross/SDK/MacOSX15.sdk/usr/include"

if ! OSXCROSS_ROOT="$tmpdir/osxcross" CPKT_OSXCROSS_HOST="$host" \
  cmake -P "$repo_root/cmake/toolchains/arm64-apple-darwin.cmake" \
  >"$tmpdir/verify-only.out" 2>"$tmpdir/verify-only.err"; then
  printf 'expected Darwin toolchain to allow verify-only setup without strip or install_name_tool\n' >&2
  cat "$tmpdir/verify-only.err" >&2
  exit 1
fi

if ! OSXCROSS_ROOT="$tmpdir/osxcross" CPKT_OSXCROSS_HOST="$host" \
  "$repo_root/scripts/osxcross_available.sh" --quiet; then
  printf 'expected Darwin release matrix availability without strip or install_name_tool\n' >&2
  exit 1
fi
OSXCROSS_ROOT="$tmpdir/osxcross" CPKT_OSXCROSS_HOST="$host" \
  "$repo_root/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin \
  >"$tmpdir/discover.out"
if grep -F "strip=$osx_bin/$host-strip" "$tmpdir/discover.out" >/dev/null; then
  printf 'Darwin discovery must not advertise a missing optional strip tool\n' >&2
  cat "$tmpdir/discover.out" >&2
  exit 1
fi

mv "$osx_bin/$host-otool" "$osx_bin/$host-otool.missing"
if OSXCROSS_ROOT="$tmpdir/osxcross" CPKT_OSXCROSS_HOST="$host" \
  cmake -P "$repo_root/cmake/toolchains/arm64-apple-darwin.cmake" \
  >"$tmpdir/missing-otool.out" 2>"$tmpdir/missing-otool.err"; then
  printf 'expected Darwin toolchain to require otool for package verification\n' >&2
  exit 1
fi
if ! grep -F 'missing CPKT_OTOOL' "$tmpdir/missing-otool.err" >/dev/null; then
  printf 'expected missing otool diagnostic, got:\n' >&2
  cat "$tmpdir/missing-otool.err" >&2
  exit 1
fi
