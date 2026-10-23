#!/usr/bin/env bash
set -euo pipefail

build_dir=$1
stage=$(mktemp -d "$build_dir/cli-package-test.XXXXXX")
trap 'rm -rf "$stage"' EXIT

cmake --install "$build_dir" --component cai-cli --prefix "$stage" >/dev/null
test -x "$stage/bin/cai"
test -f "$stage/share/man/man1/cai.1"
test -f "$stage/share/doc/libcai/README.md"
test -f "$stage/share/doc/libcai/LICENSE"
test -f "$stage/share/doc/libcai/docs/model-metadata.md"
test -f "$stage/share/doc/libcai/docs/embedding.md"
test -f "$stage/share/doc/libcai/examples/main.c"
test -f "$stage/share/doc/libcai/examples/app.lua"
for dependency in liblockdc liblql softline libmdf libpslog liblonejson curl openssl zlib nghttp2 libssh2; do
  test -f "$stage/share/doc/libcai/third_party/$dependency/LICENSE"
done
for notice in LICENSE.md LICENSE.BSD-3-Clause AUTHORS README.softline.md provenance.json softline.patch; do
  test -f "$stage/share/doc/libcai/third_party/softline/licenses/c-ares/$notice"
done
source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
curl_license="$stage/share/doc/libcai/third_party/curl/LICENSE"
mv "$curl_license" "$curl_license.saved"
if (
  CAI_REPO_ROOT="$source_root" source "$source_root/scripts/verify_release_artifacts.sh" --self-test
  verify_cli_archive "$stage" x86_64-linux-musl
) >"$stage/verify.stdout" 2>"$stage/verify.stderr"; then
  printf 'CLI artifact verification accepted a missing curl license\n' >&2
  exit 1
fi
grep -Fq "missing required file: $curl_license" "$stage/verify.stderr"
mv "$curl_license.saved" "$curl_license"
test ! -e "$stage/share/doc/cai"

if command -v groff >/dev/null; then
  groff -Tutf8 -man "$stage/share/man/man1/cai.1" >/dev/null
fi
