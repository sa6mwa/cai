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
test ! -e "$stage/share/doc/cai"

if command -v groff >/dev/null; then
  groff -Tutf8 -man "$stage/share/man/man1/cai.1" >/dev/null
fi
