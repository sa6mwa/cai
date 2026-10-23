#!/usr/bin/env bash
set -euo pipefail
repo=$1
scratch=$(mktemp -d "$2/cross-smoke-contract.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/sysroot/lib"
cat >"$scratch/cli" <<'SH'
#!/bin/sh
case "$1" in
  --version) printf '%s\n' "${CAI_FIXTURE_VERSION:-cai 1.2.3}" ;;
  --help) printf '%s\n' "${CAI_FIXTURE_HELP:-Usage: cai}" ;;
  --cai-invalid-smoke-option)
    printf '%s\n' 'unknown option' >&2
    exit "${CAI_FIXTURE_EXIT:-2}" ;;
  *) exit 3 ;;
esac
SH
# Run the real assertion script with a deterministic process fixture.
runner="$(command -v cmake);-E;env"
# Keep argument boundaries in the CMake runner list.
run_smoke() {
  cmake "-DCAI_BINARY=$scratch/cli" "-DCAI_RUNNER=$runner;$1;sh" \
    -DCAI_EXPECTED_VERSION=1.2.3 -P "$repo/tests/cross_cli_smoke.cmake"
}
run_smoke CAI_FIXTURE_VERSION=1.2.3 >"$scratch/out" 2>"$scratch/err"
for override in CAI_FIXTURE_VERSION=9.9.9 CAI_FIXTURE_VERSION=prefix1.2.3suffix CAI_FIXTURE_HELP=invalid CAI_FIXTURE_EXIT=0 CAI_FIXTURE_EXIT=7; do
  if run_smoke "$override" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'CMake Error' "$scratch/err"
done
if cmake "-DCAI_BINARY=$scratch/missing" -DCAI_RUNNER=sh \
    -DCAI_EXPECTED_VERSION=1.2.3 -P "$repo/tests/cross_cli_smoke.cmake" \
    >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
if cmake "-DCAI_BINARY=$scratch/cli" "-DCAI_RUNNER=$scratch/missing" \
    -DCAI_EXPECTED_VERSION=1.2.3 -P "$repo/tests/cross_cli_smoke.cmake" \
    >"$scratch/out" 2>"$scratch/err"; then exit 1; fi

# Missing required execution prerequisites fail before test registration.
printf 'include("%s/cmake/CaiCrossTests.cmake")\n' "$repo" >"$scratch/prerequisite.cmake"
for target in aarch64-linux-musl armhf-linux-gnu; do
  for missing in runner sysroot; do
    qemu=$(command -v cmake)
    sysroot="$scratch/sysroot"
    if [[ "$missing" == runner ]]; then qemu="$scratch/missing"; else sysroot="$scratch/missing"; fi
    if cmake "-DCAI_TARGET_ID=$target" "-DCAI_QEMU_EXECUTABLE=$qemu" \
        "-DCMAKE_SYSROOT=$sysroot" -P "$scratch/prerequisite.cmake" \
        >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
    grep -q 'ARM runtime checks require' "$scratch/err"
  done
done
