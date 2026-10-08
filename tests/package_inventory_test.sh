#!/usr/bin/env bash
set -euo pipefail
repo=$1; scratch=$(mktemp -d "$2/package-inventory.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/dist"
version=1.2.3
names=(cai-1.2.3-x86_64-linux-gnu.tar.gz cai-1.2.3.tar.gz cai-lua-1.2.3.tar.gz cai-cli-1.2.3-x86_64-linux-musl.tar.gz cai-1.2.3-1.rockspec cai-1.2.3-1.src.rock)
run() { cmake "-DCAI_DIST_DIR=$scratch/dist" "-DCAI_VERSION=$version" -DCAI_TARGETS=x86_64-linux-gnu -P "$repo/cmake/package_checksums.cmake"; }
for name in "${names[@]}"; do printf 'fixture %s\n' "$name" >"$scratch/dist/$name"; done
run
[[ $(wc -l <"$scratch/dist/cai-$version-CHECKSUMS") == ${#names[@]} ]]
(cd "$scratch/dist" && sha256sum -c "cai-$version-CHECKSUMS")
cp "$scratch/dist/cai-$version-CHECKSUMS" "$scratch/manifest"
rm "$scratch/dist/${names[0]}"
if run >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
grep -q 'Missing/nonregular selected artifact' "$scratch/err"
# The final verifier rejects a checksum-valid but incomplete inventory.
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
export CAI_RELEASE_TARGETS=x86_64-linux-gnu
require_complete_manifest "${names[@]}"
if (require_complete_manifest "${names[@]:1}") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi )
