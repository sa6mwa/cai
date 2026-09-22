#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
  printf 'usage: %s <nm> <library> <allowlist> <linux|darwin> [cc]\n' "$0" >&2
  exit 2
fi

nm=$1
library=$2
allowlist=$3
platform=$4
cc=${5:-}

fail() {
  printf 'shared_export_policy_test.sh: %s\n' "$*" >&2
  exit 1
}

[[ -x "$nm" ]] || fail "nm is not executable: $nm"
[[ -f "$library" ]] || fail "library does not exist: $library"
[[ -f "$allowlist" ]] || fail "allowlist does not exist: $allowlist"
[[ "$platform" == linux || "$platform" == darwin ]] || \
  fail "unsupported platform: $platform"

workspace=$(mktemp -d "$(dirname "$library")/.cai-export-policy.XXXXXX")
trap 'rm -rf "$workspace"' EXIT
expected="$workspace/expected"
actual="$workspace/actual"

LC_ALL=C grep -Ev '^[[:space:]]*(#|$)' "$allowlist" >"$expected"
[[ -s "$expected" ]] || fail "allowlist is empty: $allowlist"
if ! LC_ALL=C sort -cu "$expected"; then
  fail "allowlist must be sorted and unique: $allowlist"
fi
if grep -Ev '^[A-Za-z_][A-Za-z0-9_]*$' "$expected" >/dev/null; then
  fail "allowlist contains an invalid symbol: $allowlist"
fi

case "$platform" in
  linux)
    "$nm" -D --defined-only "$library" |
      awk '{ symbol = $NF; sub(/@.*/, "", symbol); if (symbol ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print symbol; }' |
      LC_ALL=C sort -u >"$actual"
    ;;
  darwin)
    "$nm" -gU "$library" |
      awk '{ symbol = $NF; sub(/^_/, "", symbol); if (symbol ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print symbol; }' |
      LC_ALL=C sort -u >"$actual"
    ;;
esac

if ! diff -u "$expected" "$actual"; then
  fail "dynamic export table differs from $allowlist: $library"
fi

if [[ -n "$cc" && "$platform" == linux ]]; then
  [[ -x "$cc" ]] || fail "C compiler is not executable: $cc"
  cat >"$workspace/private_export_probe.c" <<'EOF_C'
extern void cai_alloc(void);

int main(void) {
  cai_alloc();
  return 0;
}
EOF_C
  if "$cc" -std=c89 -Werror "$workspace/private_export_probe.c" \
      "-L$(dirname "$library")" -lcai \
      -Wl,--unresolved-symbols=ignore-in-shared-libs \
      "-Wl,-rpath,$(dirname "$library")" \
      -o "$workspace/private_export_probe" \
      >"$workspace/private_export_probe.out" 2>&1; then
    fail "a downstream consumer linked private symbol cai_alloc"
  fi
  if ! grep -F 'cai_alloc' "$workspace/private_export_probe.out" >/dev/null; then
    cat "$workspace/private_export_probe.out" >&2
    fail "private export probe did not reach cai_alloc"
  fi
fi
