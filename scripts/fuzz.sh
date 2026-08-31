#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ( "$1" != smoke && "$1" != long ) ]]; then
  printf 'usage: %s {smoke|long}\n' "$0" >&2
  exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mode=$1
seconds=${CAI_FUZZ_SECONDS:-10}
if [[ "$mode" == long ]]; then
  seconds=${CAI_FUZZ_LONG_SECONDS:-120}
fi

description=$("$repo_root/scripts/cpkt-aflpp.sh" discover)
afl_fuzz=$(sed -n 's/^afl_fuzz=//p' <<<"$description" | tail -1)
if [[ -z "$afl_fuzz" || ! -x "$afl_fuzz" ]]; then
  printf 'Pinned AFL++ resolver did not provide afl-fuzz.\n' >&2
  exit 2
fi

mkdir -p "$repo_root/build/fuzz/afl"

for target in tool stream response mcp session todo patch; do
  binary="$repo_root/build/fuzz/cai_${target}_fuzz"
  corpus="$repo_root/tests/fuzz-corpus/$target"
  output="$repo_root/build/fuzz/afl/$target-$mode"
  if [[ ! -x "$binary" || ! -d "$corpus" ]]; then
    printf 'Fuzz target or corpus is missing: target=%s binary=%s corpus=%s\n' \
      "$target" "$binary" "$corpus" >&2
    exit 2
  fi
  rm -rf "$output"
  AFL_SKIP_CPUFREQ=1 AFL_NO_AFFINITY=1 "$afl_fuzz" -V "$seconds" \
    -i "$corpus" -o "$output" -- "$binary" @@
done
