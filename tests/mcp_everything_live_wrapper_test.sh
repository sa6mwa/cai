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
cp "$repo_root/scripts/run_mcp_everything_live_test.sh" \
  "$fixture_root/scripts/run_mcp_everything_live_test.sh"

cat >"$fixture_root/scripts/compose.sh" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' "$*" >>"$CAI_MCP_WRAPPER_COMPOSE_LOG"
if [ "$1" = -p ] && [ "$3" = ps ] && [ "$4" = -aq ] && [ "$5" = mcp-everything ]; then
  printf '%s\n' "${CAI_MCP_WRAPPER_EXISTING_SERVICE:-}"
fi
EOF
chmod +x "$fixture_root/scripts/compose.sh"

cat >"$fixture_root/bin/curl" <<'EOF'
#!/bin/sh
set -eu

body=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o)
      shift
      body=$1
      ;;
  esac
  last=$1
  shift
done
printf '%s\n' "$last" >>"$CAI_MCP_WRAPPER_CURL_LOG"
printf '{"serverInfo":{"name":"everything"}}\n' >"$body"
EOF
chmod +x "$fixture_root/bin/curl"

cat >"$fixture_root/bin/seq" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' 'seq must not be required by run_mcp_everything_live_test.sh' >&2
exit 127
EOF
chmod +x "$fixture_root/bin/seq"

cat >"$fixture_root/bin/timeout" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' 'timeout must not be required by run_mcp_everything_live_test.sh' >&2
exit 127
EOF
chmod +x "$fixture_root/bin/timeout"

cat >"$fixture_root/test-binary" <<'EOF'
#!/bin/sh
set -eu

printf '%s\n' "$CAI_MCP_EVERYTHING_BASE_URL" >>"$CAI_MCP_WRAPPER_TEST_LOG"
EOF
chmod +x "$fixture_root/test-binary"

run_wrapper() {
  CAI_MCP_WRAPPER_COMPOSE_LOG=$fixture_root/compose.log \
    CAI_MCP_WRAPPER_CURL_LOG=$fixture_root/curl.log \
    CAI_MCP_WRAPPER_TEST_LOG=$fixture_root/test.log \
    CAI_MCP_EVERYTHING_COMPOSE_PROJECT=cai-mcp-wrapper-test \
    PATH="$fixture_root/bin:$PATH" \
    "$fixture_root/scripts/run_mcp_everything_live_test.sh" \
      "$fixture_root/test-binary"
}

expect_log() {
  log=$1
  pattern=$2
  if ! grep -F -- "$pattern" "$log" >/dev/null; then
    printf 'missing log entry: %s\n' "$pattern" >&2
    cat "$log" >&2
    exit 1
  fi
}

: >"$fixture_root/compose.log"
: >"$fixture_root/curl.log"
: >"$fixture_root/test.log"
if run_wrapper; then
  :
else
  printf '%s\n' 'wrapper failed on managed default service path' >&2
  exit 1
fi
expect_log "$fixture_root/compose.log" '-p cai-mcp-wrapper-test ps -aq mcp-everything'
expect_log "$fixture_root/compose.log" '-p cai-mcp-wrapper-test up -d --build mcp-everything'
expect_log "$fixture_root/compose.log" '-p cai-mcp-wrapper-test stop mcp-everything'
expect_log "$fixture_root/compose.log" '-p cai-mcp-wrapper-test rm -f mcp-everything'

: >"$fixture_root/compose.log"
: >"$fixture_root/curl.log"
: >"$fixture_root/test.log"
if CAI_MCP_EVERYTHING_BASE_URL=http://127.0.0.1:39999/mcp \
  run_wrapper; then
  :
else
  printf '%s\n' 'wrapper failed on external base URL path' >&2
  exit 1
fi
if [ -s "$fixture_root/compose.log" ]; then
  printf '%s\n' 'wrapper must not invoke Compose for explicit base URL' >&2
  cat "$fixture_root/compose.log" >&2
  exit 1
fi
expect_log "$fixture_root/test.log" 'http://127.0.0.1:39999/mcp'

: >"$fixture_root/compose.log"
: >"$fixture_root/curl.log"
: >"$fixture_root/test.log"
if CAI_MCP_EVERYTHING_PORT=45678 run_wrapper; then
  :
else
  printf '%s\n' 'wrapper failed on closed explicit port path' >&2
  exit 1
fi
expect_log "$fixture_root/compose.log" '-p cai-mcp-wrapper-test up -d --build mcp-everything'
expect_log "$fixture_root/test.log" 'http://127.0.0.1:45678/mcp'
