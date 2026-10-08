#!/usr/bin/env bash
set -euo pipefail
source_root=$1
binary=$2
strip_tool=$3
scratch=$(mktemp -d "$binary/package-repro.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
package() {
  cmake "-DCAI_ROOT=$source_root" "-DCAI_BINARY_DIR=$binary" "-DCAI_DIST_DIR=$scratch/dist" -DCAI_VERSION=0.0.0 -DCAI_TARGET_ID=x86_64-linux-gnu "-DCAI_STRIP_BIN=$strip_tool" -P "$source_root/cmake/package_archive.cmake" >"$scratch/log" 2>&1
}
(umask 022; package)
sha256sum "$scratch/dist/cai-0.0.0-x86_64-linux-gnu.tar.gz" >"$scratch/hash"
(umask 077; package)
sha256sum -c "$scratch/hash"
