#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'usage: %s <cai-tests-binary> <expected-list>\n' "$0" >&2
  exit 1
fi

cai_tests=$1
expected_list=$2

if [[ ! -x "$cai_tests" ]]; then
  printf 'missing cai_tests executable: %s\n' "$cai_tests" >&2
  exit 1
fi
if [[ ! -f "$expected_list" ]]; then
  printf 'missing expected unit test list: %s\n' "$expected_list" >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

actual_sorted=$tmpdir/actual.sorted
expected_sorted=$tmpdir/expected.sorted
missing=$tmpdir/missing
extra=$tmpdir/extra

"$cai_tests" --list | sed '/^$/d' | sort >"$actual_sorted"
sed '/^$/d' "$expected_list" | sort >"$expected_sorted"

comm -23 "$actual_sorted" "$expected_sorted" >"$missing"
comm -13 "$actual_sorted" "$expected_sorted" >"$extra"

if [[ -s "$missing" || -s "$extra" ]]; then
  if [[ -s "$missing" ]]; then
    printf '%s\n' 'CTest is missing unit tests from cai_tests --list:' >&2
    sed 's/^/  /' "$missing" >&2
  fi
  if [[ -s "$extra" ]]; then
    printf '%s\n' 'CTest registers unit tests that cai_tests --list does not expose:' >&2
    sed 's/^/  /' "$extra" >&2
  fi
  exit 1
fi
