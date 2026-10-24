#!/usr/bin/env bash
set -euo pipefail
cxx=$1 source_dir=$2 binary_dir=$3 lonejson_include=$4 library=$5
work=$(mktemp -d "$binary_dir/quota-linkage.XXXXXX")
trap 'rm -rf "$work"' EXIT
for core_first in 0 1; do
  flags=()
  if [[ "$core_first" == 1 ]]; then flags+=(-DCAI_TEST_INCLUDE_CORE_FIRST); fi
  "$cxx" -std=c++98 -Wall -Wextra -Werror "${flags[@]}" \
    -I"$source_dir/include" -I"$binary_dir/generated/include" \
    -I"$lonejson_include" -x c++ "$source_dir/tests/quota_linkage_test.c" \
    -x none "$library" -Wl,-rpath,"$(dirname "$library")" -o "$work/consumer"
  "$work/consumer"
done
