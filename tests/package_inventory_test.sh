#!/usr/bin/env bash
set -euo pipefail
repo=$1; scratch=$(mktemp -d "$2/package-inventory.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/dist"
version=1.2.3
names=(cai-1.2.3-x86_64-linux-gnu.tar.gz cai-1.2.3.tar.gz cai-lua-1.2.3.tar.gz cai-cli-1.2.3-x86_64-linux-musl.tar.gz cai-1.2.3-1.rockspec cai-1.2.3-1.src.rock)
run() { cmake "-DCAI_DIST_DIR=$scratch/dist" "-DCAI_VERSION=$version" -DCAI_TARGETS=x86_64-linux-gnu "$@" -P "$repo/cmake/package_checksums.cmake"; }
for name in "${names[@]}"; do printf 'fixture %s\n' "$name" >"$scratch/dist/$name"; done
run
[[ $(wc -l <"$scratch/dist/cai-$version-CHECKSUMS") == ${#names[@]} ]]
(cd "$scratch/dist" && sha256sum -c "cai-$version-CHECKSUMS")
cp "$scratch/dist/cai-$version-CHECKSUMS" "$scratch/manifest"
for argument in '-DCAI_TARGETS=' '-DCAI_TARGETS=unknown' '-DCAI_PACKAGE_SCOPE=unknown'; do
  if run "$argument" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
done
for path in '' "$scratch/dist/cai-$version-CHECKSUMS"; do
  if run -DCAI_PACKAGE_SCOPE=binary "-DCAI_CHECKSUMS_PATH=$path" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
done
ln -s "$scratch/dist/cai-$version-CHECKSUMS" "$scratch/manifest-alias"
if run -DCAI_PACKAGE_SCOPE=binary "-DCAI_CHECKSUMS_PATH=$scratch/manifest-alias" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
run -DCAI_PACKAGE_SCOPE=binary "-DCAI_CHECKSUMS_PATH=$scratch/binary-CHECKSUMS"
[[ $(wc -l <"$scratch/binary-CHECKSUMS") == 1 ]]
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
rm "$scratch/dist/${names[0]}"
if run >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
grep -q 'Missing/nonregular selected artifact' "$scratch/err"
# The final verifier rejects a checksum-valid but incomplete inventory.
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
export CAI_RELEASE_TARGETS=x86_64-linux-gnu
require_complete_manifest "${names[@]}"
if (require_complete_manifest "${names[@]:1}") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi )
# All six Linux SDKs must ship the static/shared app surface.
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
verify_application_sdk "$scratch/sdk" arm64-apple-darwin
paths=(include/cai/app.h lib/libcai_app.a lib/libcai_app.so
  lib/pkgconfig/cai-app.pc lib/cmake/cai_app/cai_app-config.cmake
  share/cai_app/dependencies.json share/doc/libcai_app/embedding.md
  share/doc/libcai_app/examples/main.c share/doc/libcai_app/examples/app.lua)
for path in "${paths[@]}"; do mkdir -p "$scratch/sdk/$(dirname "$path")"; touch "$scratch/sdk/$path"; done
for target in x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl; do
  verify_application_sdk "$scratch/sdk" "$target"
  for path in "${paths[@]}"; do
    mv "$scratch/sdk/$path" "$scratch/missing"
    if (verify_application_sdk "$scratch/sdk" "$target") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
    grep -Fq "missing required file: $scratch/sdk/$path" "$scratch/err"
    mv "$scratch/missing" "$scratch/sdk/$path"
  done
done )
