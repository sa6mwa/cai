#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <registration-script>\n' "$0" >&2
  exit 1
fi

registration_script=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

fake_cai_tests=$tmpdir/cai_tests
expected_list=$tmpdir/expected.txt
output_log=$tmpdir/output.log

cat >"$fake_cai_tests" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} != "--list" ]]; then
  printf 'unexpected args\n' >&2
  exit 2
fi
printf 'alpha\nextra_from_binary\n'
EOF
chmod +x "$fake_cai_tests"

cat >"$expected_list" <<'EOF'
alpha
missing_from_ctest
EOF

if "$registration_script" "$fake_cai_tests" "$expected_list" \
    >"$output_log" 2>&1; then
  printf 'registration drift fixture should fail\n' >&2
  exit 1
fi

if ! grep -Fq 'CTest is missing unit tests from cai_tests --list:' "$output_log"; then
  printf 'missing-side diagnostic was not emitted\n' >&2
  cat "$output_log" >&2
  exit 1
fi
if ! grep -Fq '  missing_from_ctest' "$output_log"; then
  printf 'missing-side diagnostic named the wrong test\n' >&2
  cat "$output_log" >&2
  exit 1
fi
if ! grep -Fq 'CTest registers unit tests that cai_tests --list does not expose:' "$output_log"; then
  printf 'extra-side diagnostic was not emitted\n' >&2
  cat "$output_log" >&2
  exit 1
fi
if ! grep -Fq '  extra_from_binary' "$output_log"; then
  printf 'extra-side diagnostic named the wrong test\n' >&2
  cat "$output_log" >&2
  exit 1
fi
