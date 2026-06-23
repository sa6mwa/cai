#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
makefile=$repo_root/Makefile

if [ ! -f "$makefile" ]; then
  printf 'Makefile not found: %s\n' "$makefile" >&2
  exit 1
fi

target_line=$(awk '
  /^mcp-everything-test:/ {
    print
    found = 1
    exit
  }
  END {
    if (!found) {
      exit 1
    }
  }
' "$makefile") || {
  printf 'mcp-everything-test target is missing\n' >&2
  exit 1
}

case " $target_line " in
  *" build-debug "*)
    ;;
  *)
    printf 'mcp-everything-test must depend on build-debug: %s\n' \
      "$target_line" >&2
    exit 1
    ;;
esac

if ! grep -F '$(CMAKE) --build build/debug --target cai_mcp_everything_e2e' \
  "$makefile" >/dev/null; then
  printf 'mcp-everything-test must build cai_mcp_everything_e2e\n' >&2
  exit 1
fi

wait_body=$(awk '
  /^mcp-everything-wait:/ {
    in_target = 1
    next
  }
  in_target && /^[^	][^:]*:/ {
    exit
  }
  in_target {
    print
  }
' "$makefile")

if printf '%s\n' "$wait_body" | grep -F '{1..30}' >/dev/null; then
  printf 'mcp-everything-wait must not use shell-specific brace expansion\n' >&2
  exit 1
fi

if ! printf '%s\n' "$wait_body" | grep -F 'while [ "$$attempt" -le 30 ]; do' \
  >/dev/null; then
  printf 'mcp-everything-wait must use a POSIX retry loop\n' >&2
  exit 1
fi
