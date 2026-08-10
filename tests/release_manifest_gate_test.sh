#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

run_gate() {
  local script=$1
  CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 bash -lc "$script"
}

write_manifest() {
  local dist=$1
  shift
  : >"$dist/cai-1.2.3-CHECKSUMS"
  local artifact
  for artifact in "$@"; do
    printf '%064d  %s\n' 0 "$artifact" >>"$dist/cai-1.2.3-CHECKSUMS"
    : >"$dist/$artifact"
  done
}

dist="$tmpdir/pass/dist"
mkdir -p "$dist"
write_manifest "$dist" \
  cai-1.2.3.tar.gz \
  cai-1.2.3-x86_64-linux-gnu.tar.gz \
  cai-lua-1.2.3.tar.gz \
  cai-1.2.3-1.rockspec \
  cai-1.2.3-1.src.rock
run_gate "
  source '$repo_root/scripts/verify_release_artifacts.sh' --self-test
  dist_dir='$dist'
  checksums=\"\$dist_dir/cai-1.2.3-CHECKSUMS\"
  mapfile -t artifacts < <(read_checksum_artifacts)
  verify_manifest_artifacts \"\${artifacts[@]}\"
  verify_dist_manifest_closure \"\${artifacts[@]}\"
"

dist="$tmpdir/missing/dist"
mkdir -p "$dist"
write_manifest "$dist" cai-1.2.3.tar.gz
: >"$dist/cai-1.2.3-x86_64-linux-gnu.tar.gz"
if run_gate "
  source '$repo_root/scripts/verify_release_artifacts.sh' --self-test
  dist_dir='$dist'
  checksums=\"\$dist_dir/cai-1.2.3-CHECKSUMS\"
  mapfile -t artifacts < <(read_checksum_artifacts)
  verify_manifest_artifacts \"\${artifacts[@]}\"
  verify_dist_manifest_closure \"\${artifacts[@]}\"
"; then
  printf 'expected unmanifested release artifact to fail verification\n' >&2
  exit 1
fi

dist="$tmpdir/stale/dist"
mkdir -p "$dist"
write_manifest "$dist" cai-1.2.3.tar.gz
: >"$dist/cai-9.9.9-x86_64-linux-gnu.tar.gz"
if run_gate "
  source '$repo_root/scripts/verify_release_artifacts.sh' --self-test
  dist_dir='$dist'
  checksums=\"\$dist_dir/cai-1.2.3-CHECKSUMS\"
  mapfile -t artifacts < <(read_checksum_artifacts)
  verify_manifest_artifacts \"\${artifacts[@]}\"
  verify_dist_manifest_closure \"\${artifacts[@]}\"
"; then
  printf 'expected stale release artifact to fail verification\n' >&2
  exit 1
fi

dist="$tmpdir/path/dist"
mkdir -p "$dist"
write_manifest "$dist" cai-1.2.3.tar.gz
printf '%064d  file://%s/dist/cai-1.2.3.tar.gz\n' 0 "$repo_root" \
  >>"$dist/cai-1.2.3-CHECKSUMS"
if run_gate "
  source '$repo_root/scripts/verify_release_artifacts.sh' --self-test
  dist_dir='$dist'
  checksums=\"\$dist_dir/cai-1.2.3-CHECKSUMS\"
  verify_no_private_manifest_paths \"\$checksums\"
"; then
  printf 'expected local file URL in checksum manifest to fail verification\n' >&2
  exit 1
fi
