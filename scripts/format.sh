#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
command -v clang-format >/dev/null || { printf 'clang-format is required\n' >&2; exit 2; }
flags=(-i)
case "${1:-write}" in write) ;; check) flags=(--dry-run --Werror) ;; *) exit 2 ;; esac
while IFS= read -r -d '' file; do clang-format "${flags[@]}" "$file"; done < <(find "$root/src" "$root/include" "$root/cli" "$root/lua" "$root/tests" "$root/examples" -type f \( -name '*.c' -o -name '*.h' -o -name '*.inc' \) ! -path "$root/examples/mike-mind/mike_mind_prompt.h" -print0)
