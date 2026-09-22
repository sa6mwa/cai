#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

fake_nm=$tmpdir/nm
cat >"$fake_nm" <<'EOF_NM'
#!/usr/bin/env bash
set -euo pipefail

while IFS= read -r symbol; do
  printf '0000000000000000 T _%s\n' "$symbol"
done <"${CAI_FAKE_EXPORTS:?}"
if [[ "${CAI_FAKE_PRIVATE_EXPORT:-0}" = 1 ]]; then
  printf '%s\n' '0000000000000000 T _cai_alloc'
fi
EOF_NM
chmod +x "$fake_nm"

root_dir=$tmpdir/pkg
mkdir -p "$root_dir/lib"
: >"$root_dir/lib/libcai.0.0.0.dylib"

CAI_REPO_ROOT=$repo_root CAI_NM=$fake_nm \
CAI_FAKE_EXPORTS=$repo_root/cmake/cai_shared.exports bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_shared_exports '$root_dir' arm64-apple-darwin"

if CAI_REPO_ROOT=$repo_root CAI_NM=$fake_nm \
  CAI_FAKE_EXPORTS=$repo_root/cmake/cai_shared.exports \
  CAI_FAKE_PRIVATE_EXPORT=1 bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_shared_exports '$root_dir' arm64-apple-darwin" \
  >/dev/null 2>&1; then
  printf 'expected extracted SDK private export to fail verification\n' >&2
  exit 1
fi
