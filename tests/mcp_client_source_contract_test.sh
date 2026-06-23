#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
source_file=$repo_root/src/cai_mcp_client.c

if [ ! -f "$source_file" ]; then
  printf 'MCP client source not found: %s\n' "$source_file" >&2
  exit 1
fi

function_body=$(awk '
  /^cai_mcp_parse_result_response\(/ {
    in_function = 1
  }
  in_function {
    print
  }
  in_function && /^}/ {
    exit
  }
' "$source_file")

if [ -z "$function_body" ]; then
  printf 'cai_mcp_parse_result_response was not found\n' >&2
  exit 1
fi

if ! printf '%s\n' "$function_body" | grep -F 'set_parse_sink' >/dev/null; then
  printf 'cai_mcp_parse_result_response must use lonejson parse-sink output\n' >&2
  exit 1
fi

if ! printf '%s\n' "$function_body" | grep -F 'cai_mcp_result_sink' >/dev/null; then
  printf 'cai_mcp_parse_result_response must stream result parse output to caller sink\n' >&2
  exit 1
fi

if printf '%s\n' "$function_body" | grep -F 'cai_mcp_spool_sink' >/dev/null; then
  printf 'cai_mcp_parse_result_response must not spool result before caller sink write\n' >&2
  exit 1
fi

if printf '%s\n' "$function_body" | grep -F 'cai_mcp_spooled_write_to_sink' >/dev/null; then
  printf 'cai_mcp_parse_result_response must not replay a spooled result to the caller sink\n' >&2
  exit 1
fi
