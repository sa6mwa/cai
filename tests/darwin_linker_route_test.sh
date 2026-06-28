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

cat >"$osx_bin/$host-clang" <<'EOF_CLANG'
#!/usr/bin/env bash
set -euo pipefail
linker=
for arg in "$@"; do
  case "$arg" in
    -fuse-ld=*)
      linker=${arg#-fuse-ld=}
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
  "$osx_bin/$host-clang" "-fuse-ld=$osx_bin/$host-ld" \
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

if ! grep -F 'set(_cai_darwin_linker_flag "-fuse-ld=${CMAKE_LINKER}")' \
  "$repo_root/cmake/toolchains/arm64-apple-darwin.cmake" >/dev/null; then
  printf 'Darwin toolchain must inject absolute -fuse-ld=${CMAKE_LINKER}\n' >&2
  exit 1
fi
