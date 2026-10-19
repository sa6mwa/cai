#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 1
fi

repo_root=$1
expected=7.8.9-test
expected_prerelease_build=7.8.9-test+build.1
expected_stable_build=7.8.9+build.1

actual=$(CAI_VERSION_OVERRIDE=$expected "$repo_root/scripts/release_version.sh" "$repo_root")
if [[ "$actual" != "$expected" ]]; then
  printf 'release_version returned %s, expected %s\n' "$actual" "$expected" >&2
  exit 1
fi
actual=$(CAI_VERSION_OVERRIDE=$expected_prerelease_build "$repo_root/scripts/release_version.sh" "$repo_root")
if [[ "$actual" != "$expected_prerelease_build" ]]; then
  printf 'release_version returned %s, expected %s\n' "$actual" "$expected_prerelease_build" >&2
  exit 1
fi
actual=$(CAI_VERSION_OVERRIDE=$expected_stable_build "$repo_root/scripts/release_version.sh" "$repo_root")
if [[ "$actual" != "$expected_stable_build" ]]; then
  printf 'release_version returned %s, expected %s\n' "$actual" "$expected_stable_build" >&2
  exit 1
fi

for invalid_version in \
  '1.2.3/foo' \
  '1.2.3";message(FATAL_ERROR injected);#' \
  '1.2.3 ' \
  '1.2.3*' \
  '01.2.3' \
  '1.02.3' \
  '1.2.03' \
  '1.2.3-' \
  '1.2.3-alpha..1' \
  '1.2.3-01' \
  '1.2.3+' \
  '1.2.3+build..1'; do
  if CAI_VERSION_OVERRIDE=$invalid_version \
    "$repo_root/scripts/release_version.sh" "$repo_root" >/dev/null 2>&1; then
    printf 'release_version accepted invalid override: %s\n' \
      "$invalid_version" >&2
    exit 1
  fi
done
if CAI_VERSION_OVERRIDE=1.2.3/foo \
  make -s -C "$repo_root" print-release-version >/dev/null 2>&1; then
  printf '%s\n' 'make print-release-version accepted an invalid release version override' >&2
  exit 1
fi
if CAI_VERSION_OVERRIDE=1.2.3/foo \
  make -n -C "$repo_root" release-lua-artifacts >/dev/null 2>&1; then
  printf '%s\n' 'make release-lua-artifacts accepted an invalid release version override' >&2
  exit 1
fi

actual=$(CAI_VERSION_OVERRIDE=$expected make -s -C "$repo_root" print-release-version)
if [[ "$actual" != "$expected" ]]; then
  printf 'make print-release-version returned %s, expected %s\n' "$actual" "$expected" >&2
  exit 1
fi

dry_run=$(CAI_VERSION_OVERRIDE=$expected make -n -C "$repo_root" release-lua-artifacts)
if grep -q 'cai-lua-0\.0\.0\|cai-0\.0\.0-1' <<<"$dry_run"; then
  printf '%s\n' 'release-lua-artifacts dry run still references 0.0.0' >&2
  exit 1
fi
if ! grep -Fq "scripts/package_lua.sh \"$expected\"" <<<"$dry_run"; then
  printf 'Lua package command did not select candidate version\n' >&2
  exit 1
fi

dry_run=$(CAI_VERSION_OVERRIDE=0.0.0 make -n -C "$repo_root" release-lua-artifacts)
if ! grep -Fq 'scripts/package_lua.sh "0.0.0"' <<<"$dry_run"; then
  printf 'Lua package command did not select untagged fallback\n' >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

# Exercise the actual resolver and package templates without configuring a
# compiler or acquiring dependencies for a version-only regression.
mkdir -p "$tmpdir/version-source"
cat >"$tmpdir/version-source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
include("${CAI_SOURCE_DIR}/cmake/CaiVersion.cmake")
cai_detect_version(CAI_PACKAGE_VERSION "${CAI_SOURCE_DIR}")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" CAI_CMAKE_PROJECT_VERSION "${CAI_PACKAGE_VERSION}")
project(version_fixture VERSION ${CAI_CMAKE_PROJECT_VERSION} LANGUAGES NONE)
set(CMAKE_SIZEOF_VOID_P 8)
configure_file("${CAI_SOURCE_DIR}/cmake/cai_version.h.in" "generated/include/cai/version.h" @ONLY)
configure_file("${CAI_SOURCE_DIR}/cmake/cai.pc.in" "cai.pc" @ONLY)
configure_file("${CAI_SOURCE_DIR}/cmake/cai-config-version.cmake.in" "cai-config-version.cmake" @ONLY)
CMAKE
cmake -S "$tmpdir/version-source" -B "$tmpdir/version-override" \
  "-DCAI_SOURCE_DIR=$repo_root" "-DCAI_VERSION_OVERRIDE=$expected" >/dev/null
if ! grep -F "#define CAI_VERSION_STRING \"$expected\"" \
  "$tmpdir/version-override/generated/include/cai/version.h" >/dev/null; then
  printf '%s\n' 'CMake version override did not reach generated package metadata' >&2
  exit 1
fi
cat >"$tmpdir/check-cmake-package-version.cmake" <<'CMAKE'
set(PACKAGE_FIND_VERSION "7.8.9")
set(PACKAGE_FIND_VERSION_EXACT TRUE)
include("${VERSION_FILE}")
if(PACKAGE_VERSION_COMPATIBLE OR PACKAGE_VERSION_EXACT)
  message(FATAL_ERROR
    "prerelease package version satisfied exact stable request")
endif()
unset(PACKAGE_FIND_VERSION_EXACT)
set(PACKAGE_FIND_VERSION "7.0.0")
set(PACKAGE_FIND_VERSION_MAJOR "7")
set(PACKAGE_FIND_VERSION_MIN_MAJOR "7")
set(PACKAGE_FIND_VERSION_RANGE TRUE)
set(PACKAGE_FIND_VERSION_RANGE_MAX "EXCLUDE")
set(PACKAGE_FIND_VERSION_MAX "7.8.0")
set(PACKAGE_FIND_VERSION_MAX_MAJOR "7")
include("${VERSION_FILE}")
if(PACKAGE_VERSION_COMPATIBLE)
  message(FATAL_ERROR "prerelease package version exceeded requested range")
endif()
set(PACKAGE_FIND_VERSION_MAX "8.0.0")
set(PACKAGE_FIND_VERSION_MAX_MAJOR "8")
include("${VERSION_FILE}")
if(NOT PACKAGE_VERSION_COMPATIBLE)
  message(FATAL_ERROR "prerelease package version did not satisfy valid range")
endif()
unset(PACKAGE_FIND_VERSION_RANGE)
unset(PACKAGE_FIND_VERSION_RANGE_MAX)
unset(PACKAGE_FIND_VERSION_MAX)
unset(PACKAGE_FIND_VERSION_MAX_MAJOR)
set(PACKAGE_FIND_VERSION "7.8.9-test")
set(PACKAGE_FIND_VERSION_EXACT TRUE)
include("${VERSION_FILE}")
if(NOT PACKAGE_VERSION_COMPATIBLE OR NOT PACKAGE_VERSION_EXACT)
  message(FATAL_ERROR "prerelease package version did not satisfy exact prerelease request")
endif()
set(CMAKE_SIZEOF_VOID_P 4)
set(PACKAGE_FIND_VERSION "")
unset(PACKAGE_FIND_VERSION_EXACT)
include("${VERSION_FILE}")
if(NOT PACKAGE_VERSION_UNSUITABLE)
  message(FATAL_ERROR "package version did not reject pointer-size mismatch")
endif()
CMAKE
cmake -DVERSION_FILE="$tmpdir/version-override/cai-config-version.cmake" \
  -P "$tmpdir/check-cmake-package-version.cmake"

cmake -S "$tmpdir/version-source" -B "$tmpdir/stable-build-metadata-override" \
  "-DCAI_SOURCE_DIR=$repo_root" "-DCAI_VERSION_OVERRIDE=$expected_stable_build" >/dev/null
cat >"$tmpdir/check-cmake-package-build-metadata.cmake" <<'CMAKE'
set(PACKAGE_FIND_VERSION "7.8.9")
set(PACKAGE_FIND_VERSION_EXACT TRUE)
include("${VERSION_FILE}")
if(NOT PACKAGE_VERSION_COMPATIBLE OR NOT PACKAGE_VERSION_EXACT)
  message(FATAL_ERROR
    "stable package version with build metadata did not satisfy exact numeric request")
endif()
set(PACKAGE_FIND_VERSION "7.0.0")
set(PACKAGE_FIND_VERSION_MAJOR "7")
unset(PACKAGE_FIND_VERSION_EXACT)
include("${VERSION_FILE}")
if(NOT PACKAGE_VERSION_COMPATIBLE)
  message(FATAL_ERROR
    "stable package version with build metadata did not satisfy same-major request")
endif()
CMAKE
cmake -DVERSION_FILE="$tmpdir/stable-build-metadata-override/cai-config-version.cmake" \
  -P "$tmpdir/check-cmake-package-build-metadata.cmake"
if cmake -S "$tmpdir/version-source" -B "$tmpdir/invalid-version-override" \
  "-DCAI_SOURCE_DIR=$repo_root" -DCAI_VERSION_OVERRIDE=1.2.3/foo >/dev/null 2>&1; then
  printf '%s\n' 'CMake accepted an invalid release version override' >&2
  exit 1
fi

git init -q "$tmpdir/repo"
git -C "$tmpdir/repo" config user.name "cai test"
git -C "$tmpdir/repo" config user.email "cai@example.invalid"
printf 'fixture\n' >"$tmpdir/repo/fixture.txt"
git -C "$tmpdir/repo" add fixture.txt
git -C "$tmpdir/repo"  -c core.hooksPath=/dev/null -c commit.gpgSign=false commit -q -m 'test(lifecycle): seed version input'
git -C "$tmpdir/repo" -c tag.gpgSign=false tag v1.2.3
git -C "$tmpdir/repo" worktree add -q "$tmpdir/worktree" HEAD

actual=$(CAI_VERSION_OVERRIDE=$expected "$repo_root/scripts/release_version.sh" "$tmpdir/worktree")
if [[ "$actual" != "$expected" ]]; then
  printf 'release_version returned %s for tagged worktree with override, expected %s\n' "$actual" "$expected" >&2
  exit 1
fi

actual=$("$repo_root/scripts/release_version.sh" "$tmpdir/worktree")
if [[ "$actual" != "1.2.3" ]]; then
  printf 'release_version returned %s for tagged worktree, expected 1.2.3\n' "$actual" >&2
  exit 1
fi

git init -q "$tmpdir/invalid-tag-repo"
git -C "$tmpdir/invalid-tag-repo" config user.name "cai test"
git -C "$tmpdir/invalid-tag-repo" config user.email "cai@example.invalid"
printf 'fixture\n' >"$tmpdir/invalid-tag-repo/fixture.txt"
git -C "$tmpdir/invalid-tag-repo" add fixture.txt
git -C "$tmpdir/invalid-tag-repo"  -c core.hooksPath=/dev/null -c commit.gpgSign=false commit -q -m 'test(lifecycle): seed version input'
git -C "$tmpdir/invalid-tag-repo" -c tag.gpgSign=false tag v1.2.3/foo
if "$repo_root/scripts/release_version.sh" \
  "$tmpdir/invalid-tag-repo" >/dev/null 2>&1; then
  printf '%s\n' 'release_version accepted an invalid exact release tag' >&2
  exit 1
fi

mkdir -p "$tmpdir/repo/extracted-source"
printf 'VERSION\nRELEASE_MANIFEST\n' >"$tmpdir/repo/extracted-source/RELEASE_MANIFEST"
printf '7.8.9\n' >"$tmpdir/repo/extracted-source/VERSION"
actual=$("$repo_root/scripts/release_version.sh" "$tmpdir/repo/extracted-source")
if [[ "$actual" != "7.8.9" ]]; then
  printf 'release_version returned %s for extracted source under git worktree, expected 7.8.9\n' "$actual" >&2
  exit 1
fi
printf '  7.8.9-test  \n' >"$tmpdir/repo/extracted-source/VERSION"
actual=$("$repo_root/scripts/release_version.sh" "$tmpdir/repo/extracted-source")
if [[ "$actual" != "7.8.9-test" ]]; then
  printf 'release_version returned %s for source VERSION with surrounding whitespace, expected 7.8.9-test\n' "$actual" >&2
  exit 1
fi
printf '1.2.3/foo\n' >"$tmpdir/repo/extracted-source/VERSION"
if "$repo_root/scripts/release_version.sh" \
  "$tmpdir/repo/extracted-source" >/dev/null 2>&1; then
  printf '%s\n' 'release_version accepted an invalid source VERSION' >&2
  exit 1
fi
printf '1.2.3-alpha beta\n' >"$tmpdir/repo/extracted-source/VERSION"
if "$repo_root/scripts/release_version.sh" \
  "$tmpdir/repo/extracted-source" >/dev/null 2>&1; then
  printf '%s\n' 'release_version accepted source VERSION with internal whitespace' >&2
  exit 1
fi

git -C "$tmpdir/repo" worktree add -q "$tmpdir/untagged-worktree" HEAD
git -C "$tmpdir/untagged-worktree" tag -d v1.2.3 >/dev/null
printf '9.9.9\n' >"$tmpdir/untagged-worktree/VERSION"

actual=$("$repo_root/scripts/release_version.sh" "$tmpdir/untagged-worktree")
if [[ "$actual" != "0.0.0" ]]; then
  printf 'release_version returned %s for untagged git worktree with VERSION, expected 0.0.0\n' "$actual" >&2
  exit 1
fi

git -C "$tmpdir/repo" -c tag.gpgSign=false tag -a v1.2.4 -m fixture
if "$repo_root/scripts/release_version.sh" "$tmpdir/worktree" >/dev/null 2>&1; then
  printf '%s\n' 'release_version accepted an annotated release tag' >&2
  exit 1
fi
