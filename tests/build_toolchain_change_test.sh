#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 || ! -d "$2" ]]; then
  printf 'usage: %s <source-root> <existing-build-dir>\n' "$0" >&2
  exit 2
fi
repo_root=$1
build_dir=$2
test_root=$(mktemp -d "$build_dir/toolchain-change.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/scripts" "$test_root/bin" "$test_root/build/fuzz"
cp "$repo_root/scripts/build.sh" "$test_root/scripts/"
cat >"$test_root/scripts/cpkt-toolchains.sh" <<'EOF'
#!/usr/bin/env bash
printf 'root=/pinned/new\n'
EOF
cat >"$test_root/scripts/cpkt-aflpp.sh" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$test_root/bin/cmake" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*"
EOF
chmod +x "$test_root/scripts/cpkt-toolchains.sh" "$test_root/scripts/cpkt-aflpp.sh" "$test_root/bin/cmake"
for root in /pinned/old /pinned/new; do
  printf 'CMAKE_TOOLCHAIN_FILE:FILEPATH=%s/cmake/toolchains/linux-aflpp.cmake\nCPKT_TARGET_ID:STRING=x86_64-linux-gnu\nCPKT_BOOTLIN_ROOT:PATH=%s\nCMAKE_C_COMPILER:FILEPATH=/afl/compiler\n' \
    "$test_root" "$root" >"$test_root/build/fuzz/CMakeCache.txt"
  actual=$(PATH="$test_root/bin:$PATH" bash "$test_root/scripts/build.sh" configure-fuzz)
  expected='--preset fuzz'
  [[ "$root" == /pinned/new ]] || expected='--fresh --preset fuzz'
  [[ "$actual" == "$expected" ]] || { printf 'Expected %s, got %s\n' "$expected" "$actual" >&2; exit 1; }
done
printf 'Compiler collection change forces a fresh preset configuration\n'
