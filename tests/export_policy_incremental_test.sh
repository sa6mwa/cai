#!/usr/bin/env bash
set -euo pipefail
repo=$1; scratch=$(mktemp -d "$2/export-incremental.XXXXXX"); cc=$3; nm=$4; cmake=$5
trap 'rm -rf "$scratch"' EXIT
printf 'one\n' >"$scratch/symbols"
printf 'int one(void); int two(void);\nint one(void) {return 1;}\nint two(void) {return 2;}\n' >"$scratch/sample.c"
cat >"$scratch/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.21)
project(Exports C)
include("$repo/cmake/cai-export-policy.cmake")
add_library(sample SHARED sample.c)
target_compile_options(sample PRIVATE -Wall -Wextra -Werror)
target_link_options(sample PRIVATE -Wl,--fatal-warnings)
cai_apply_export_policy(sample "\${CMAKE_CURRENT_SOURCE_DIR}/symbols")
CMAKE
configure() { "$cmake" -S "$scratch" -B "$scratch/build" -G Ninja "-DCMAKE_C_COMPILER=$cc"; }
build() { "$cmake" --build "$scratch/build" --target sample --parallel 1; }
exports() { "$nm" -D --defined-only "$scratch/build/libsample.so" | awk '{print $NF}' | sort; }
configure; build; [[ $(exports) == one ]]
times=$(stat -c '%y' "$scratch/build/libsample.so" "$scratch/build/sample.export-map")
configure; build; [[ $(stat -c '%y' "$scratch/build/libsample.so" "$scratch/build/sample.export-map") == "$times" ]]
printf 'one\ntwo\n' >"$scratch/symbols"; build; [[ $(exports) == $'one\ntwo' ]]
printf 'two\n' >"$scratch/symbols"; build; [[ $(exports) == two ]]
stamp=$(stat -c '%y' "$scratch/build/libsample.so")
printf 'two\ntwo\n' >"$scratch/symbols"
if build >"$scratch/log" 2>&1; then exit 1; fi
grep -q 'duplicate symbol' "$scratch/log"
[[ $(stat -c '%y' "$scratch/build/libsample.so") == "$stamp" ]]
