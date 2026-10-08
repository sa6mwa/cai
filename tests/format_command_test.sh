#!/usr/bin/env bash
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/format-command.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
checkout="$scratch/checkout with spaces"
mkdir -p "$checkout/"{scripts,src,include,cli,lua,tests,examples/mike-mind} "$scratch/bin"
cp "$repo/scripts/format.sh" "$checkout/scripts/"
for file in 'src/file with spaces.c' include/api.h lua/binding.inc cli/main.c examples/mike-mind/mike_mind_prompt.h src/other.cpp; do
  printf 'fixture\n' >"$checkout/$file"
done
cat >"$scratch/bin/clang-format" <<'SH'
#!/usr/bin/env bash
printf 'invocation\n' >>"$CAI_FORMAT_LOG"
printf '%s\n' "$@" >>"$CAI_FORMAT_LOG"
exit "${CAI_FORMAT_EXIT:-0}"
SH
chmod +x "$scratch/bin/clang-format"
export PATH="$scratch/bin:$PATH" CAI_FORMAT_LOG="$scratch/events" CAI_FORMAT_EXIT=0
for mode in write check; do
  : >"$CAI_FORMAT_LOG"
  bash "$checkout/scripts/format.sh" "$mode"
  [[ $(grep -c '^invocation$' "$CAI_FORMAT_LOG") == 1 ]]
  grep -Fxq "$checkout/src/file with spaces.c" "$CAI_FORMAT_LOG"
  grep -Fxq "$checkout/lua/binding.inc" "$CAI_FORMAT_LOG"
  ! grep -q 'other.cpp\|mike_mind_prompt.h' "$CAI_FORMAT_LOG"
  if [[ "$mode" == check ]]; then grep -qx -- --dry-run "$CAI_FORMAT_LOG"; grep -qx -- --Werror "$CAI_FORMAT_LOG"
  else grep -qx -- -i "$CAI_FORMAT_LOG"; fi
done
if CAI_FORMAT_EXIT=7 bash "$checkout/scripts/format.sh" check >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
mv "$checkout/src" "$scratch/src-away"
if bash "$checkout/scripts/format.sh" check >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ -s "$scratch/err" ]]
mv "$scratch/src-away" "$checkout/src"
: >"$CAI_FORMAT_LOG"
if bash "$checkout/scripts/format.sh" unknown >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
[[ ! -s "$CAI_FORMAT_LOG" ]]
