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

extract_function() {
  awk -v name="$1" '
    function brace_delta(line, tmp) {
      tmp = line
      return gsub(/\{/, "{", tmp) - gsub(/\}/, "}", tmp)
    }

    !in_function && !candidate && index($0, name "(") != 0 {
      candidate = 1
      pending = $0 "\n"
      if (index($0, "{") != 0) {
        in_function = 1
        depth = brace_delta($0)
        printf "%s", pending
        pending = ""
        if (depth == 0) {
          exit
        }
      } else if (index($0, ";") != 0) {
        candidate = 0
        pending = ""
      }
      next
    }

    candidate && !in_function {
      pending = pending $0 "\n"
      if (index($0, "{") != 0) {
        in_function = 1
        depth = brace_delta($0)
        printf "%s", pending
        pending = ""
        if (depth == 0) {
          exit
        }
      } else if (index($0, ";") != 0) {
        candidate = 0
        pending = ""
      }
      next
    }

    in_function {
      print
      depth += brace_delta($0)
      if (depth == 0) {
        exit
      }
    }
  ' "$source_file"
}

function_body=$(extract_function cai_mcp_parse_result_response)

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

helper_body=$(extract_function cai_mcp_configure_curl_common)

if [ -z "$helper_body" ]; then
  printf 'cai_mcp_configure_curl_common was not found\n' >&2
  exit 1
fi

if ! printf '%s\n' "$helper_body" | grep -F 'CURLOPT_NOSIGNAL' >/dev/null; then
  printf 'MCP curl common configuration must set CURLOPT_NOSIGNAL\n' >&2
  exit 1
fi

for function_name in \
  cai_mcp_post_ex \
  cai_mcp_streaming_post_thread \
  cai_mcp_get_resume_response
do
  curl_body=$(extract_function "$function_name")

  if [ -z "$curl_body" ]; then
    printf '%s was not found\n' "$function_name" >&2
    exit 1
  fi

  if ! printf '%s\n' "$curl_body" | grep -F 'curl_easy_perform' >/dev/null; then
    printf '%s no longer performs a curl request; update this contract test\n' \
      "$function_name" >&2
    exit 1
  fi

  if ! printf '%s\n' "$curl_body" | grep -F 'cai_mcp_configure_curl_common' >/dev/null; then
    printf '%s must use cai_mcp_configure_curl_common before curl_easy_perform\n' \
      "$function_name" >&2
    exit 1
  fi

  configure_line=$(printf '%s\n' "$curl_body" |
    awk '/cai_mcp_configure_curl_common/ { print NR; exit }')
  perform_line=$(printf '%s\n' "$curl_body" |
    awk '/curl_easy_perform/ { print NR; exit }')
  if [ "$configure_line" -ge "$perform_line" ]; then
    printf '%s must configure MCP curl options before curl_easy_perform\n' \
      "$function_name" >&2
    exit 1
  fi
done
