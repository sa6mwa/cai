#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

fake_otool=$tmpdir/otool
cat >"$fake_otool" <<'EOF_OTOOL'
#!/usr/bin/env bash
set -euo pipefail
mode=$1
case "$mode" in
  -D)
    printf '%s:\n' "$2"
    printf '%s\n' "${CAI_FAKE_INSTALL_NAME:-@rpath/libcai.2.dylib}"
    ;;
  -L)
    printf '%s:\n' "$2"
    printf '\t%s (compatibility version 2.0.0, current version 2.0.0)\n' \
      "${CAI_FAKE_INSTALL_NAME:-@rpath/libcai.2.dylib}"
    printf '\t%s (compatibility version 19.0.0, current version 19.0.0)\n' \
      "${CAI_FAKE_DEPENDENCY:-@rpath/liblonejson.19.dylib}"
    printf '\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n'
    ;;
  -l)
    printf 'Load command 0\n'
    printf '          cmd LC_ID_DYLIB\n'
    printf 'Load command 1\n'
    printf '          cmd LC_RPATH\n'
    printf '      cmdsize 32\n'
    printf '         path %s (offset 12)\n' "${CAI_FAKE_RPATH:-@loader_path}"
    ;;
  *)
    exit 2
    ;;
esac
EOF_OTOOL
chmod +x "$fake_otool"

root_dir=$tmpdir/pkg/lib
mkdir -p "$root_dir"
: >"$root_dir/libcai.0.2.0.dylib"

CAI_REPO_ROOT=$repo_root CAI_OTOOL=$fake_otool bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_darwin_runpath '$tmpdir/pkg' arm64-apple-darwin"

if CAI_REPO_ROOT=$repo_root CAI_OTOOL=$fake_otool CAI_FAKE_INSTALL_NAME=/tmp/libcai.dylib bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_darwin_runpath '$tmpdir/pkg' arm64-apple-darwin" \
  >/dev/null 2>&1; then
  printf 'expected non-@rpath Darwin install name to fail\n' >&2
  exit 1
fi

if CAI_REPO_ROOT=$repo_root CAI_OTOOL=$fake_otool CAI_FAKE_DEPENDENCY=/lib/libbad.dylib bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_darwin_runpath '$tmpdir/pkg' arm64-apple-darwin" \
  >/dev/null 2>&1; then
  printf 'expected non-system absolute Darwin dependency to fail\n' >&2
  exit 1
fi

if CAI_REPO_ROOT=$repo_root CAI_OTOOL=$fake_otool CAI_FAKE_RPATH=/tmp/cai bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_darwin_runpath '$tmpdir/pkg' arm64-apple-darwin" \
  >/dev/null 2>&1; then
  printf 'expected local Darwin rpath to fail\n' >&2
  exit 1
fi
