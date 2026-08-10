#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
fixture_root=$(mktemp -d)

cleanup() {
  rm -rf "$fixture_root"
}
trap cleanup EXIT INT TERM

mkdir -p "$fixture_root/scripts" "$fixture_root/bin"
cp "$repo_root/scripts/test-e2e.sh" "$fixture_root/scripts/test-e2e.sh"

cat >"$fixture_root/scripts/compose.sh" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' "$*" >>"$CAI_E2E_COMPOSE_LOG"
if [ "$1" = -p ] && [ "$3" = ps ] && [ "$4" = -aq ] && [ "$5" = mcp-everything ]; then
  printf '%s\n' "${CAI_E2E_EXISTING_SERVICE:-}"
fi
EOF
chmod +x "$fixture_root/scripts/compose.sh"

cat >"$fixture_root/bin/make" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' "$*" >>"$CAI_E2E_MAKE_LOG"
EOF
chmod +x "$fixture_root/bin/make"

cat >"$fixture_root/bin/seq" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' 'seq must not be required by test-e2e.sh' >&2
exit 127
EOF
chmod +x "$fixture_root/bin/seq"

cat >"$fixture_root/bin/timeout" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' 'timeout must not be required by test-e2e.sh' >&2
exit 127
EOF
chmod +x "$fixture_root/bin/timeout"

run_e2e() {
  CAI_E2E_EXISTING_SERVICE="${CAI_E2E_EXISTING_SERVICE:-}" \
    CAI_E2E_COMPOSE_LOG=$fixture_root/compose.log \
    CAI_E2E_MAKE_LOG=$fixture_root/make.log \
    CAI_E2E_COMPOSE_PROJECT=cai-e2e-test \
    CAI_MCP_EVERYTHING_PORT=34567 \
    PATH="$fixture_root/bin:$PATH" \
    "$fixture_root/scripts/test-e2e.sh"
}

run_e2e_dynamic_port() {
  CAI_E2E_EXISTING_SERVICE="${CAI_E2E_EXISTING_SERVICE:-}" \
    CAI_E2E_COMPOSE_LOG=$fixture_root/compose.log \
    CAI_E2E_MAKE_LOG=$fixture_root/make.log \
    CAI_E2E_COMPOSE_PROJECT=cai-e2e-test \
    PATH="$fixture_root/bin:$PATH" \
    "$fixture_root/scripts/test-e2e.sh"
}

expect_log() {
  pattern=$1
  if ! grep -F -- "$pattern" "$fixture_root/compose.log" >/dev/null; then
    printf 'missing compose invocation: %s\n' "$pattern" >&2
    cat "$fixture_root/compose.log" >&2
    exit 1
  fi
}

if CAI_E2E_EXISTING_SERVICE=existing-service run_e2e; then
  :
else
  printf '%s\n' 'e2e script failed with an existing service' >&2
  exit 1
fi
expect_log '-p cai-e2e-test ps -aq mcp-everything'
expect_log '-p cai-e2e-test up -d --build mcp-everything'
if grep -E '^-p cai-e2e-test (stop|rm -f) mcp-everything$' "$fixture_root/compose.log" >/dev/null; then
  printf '%s\n' 'e2e script must not stop or remove a pre-existing service' >&2
  cat "$fixture_root/compose.log" >&2
  exit 1
fi

: >"$fixture_root/compose.log"
: >"$fixture_root/make.log"
if CAI_E2E_EXISTING_SERVICE= run_e2e; then
  :
else
  printf '%s\n' 'e2e script failed when it owned the service' >&2
  exit 1
fi
expect_log '-p cai-e2e-test stop mcp-everything'
expect_log '-p cai-e2e-test rm -f mcp-everything'

: >"$fixture_root/compose.log"
: >"$fixture_root/make.log"
if CAI_E2E_EXISTING_SERVICE= run_e2e_dynamic_port; then
  :
else
  printf '%s\n' 'e2e script failed during dynamic port selection' >&2
  exit 1
fi
expect_log '-p cai-e2e-test up -d --build mcp-everything'
