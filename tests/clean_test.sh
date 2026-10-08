#!/usr/bin/env bash
# Prove cleanup scope using disposable generated state and borrowed cache data.
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/clean-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "cleanup regression failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; cat "$scratch/err" >&2 2>/dev/null || true' ERR
checkout="$scratch/checkout with spaces"
mkdir -p "$checkout/scripts" "$checkout/cmake" "$scratch/shared"
cp "$repo/scripts/clean.sh" "$checkout/scripts/"
cp "$repo/cmake/clean_check.cmake" "$checkout/cmake/"
cat >"$checkout/scripts/devenv.sh" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$CAI_CLEAN_SERVICE_LOG"
[[ ${CAI_CLEAN_TEARDOWN_FAIL:-0} != 1 ]]
SH
chmod +x "$checkout/scripts/devenv.sh"
export CAI_CLEAN_SERVICE_LOG="$scratch/services"
printf borrowed >"$scratch/shared/retained"
# Isolate declared cache inputs from the surrounding developer environment.
export CPKT_DEPENDENCY_CACHE= CPKT_TOOLCHAIN_CACHE=
seed() {
  mkdir -p "$checkout/build/debug" "$checkout/dist/shared" "$checkout/.cache/deps"
  for directory in build dist .cache; do printf owned >"$checkout/$directory/retained"; done
}
reject() {
  if bash "$checkout/scripts/clean.sh" "$@" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'cache overlaps disposable' "$scratch/err"
  for directory in build dist .cache; do [[ -f "$checkout/$directory/retained" ]]; done
}
seed
# Environment and configured cache inputs block unsafe removal before mutation.
CPKT_DEPENDENCY_CACHE="$checkout/dist/shared" reject dist
CPKT_TOOLCHAIN_CACHE="$checkout/build/tools" reject all
printf 'CPKT_DEPENDENCY_CACHE:PATH=%s\n' "$checkout/dist/shared" >"$checkout/build/debug/CMakeCache.txt"
reject dist
rm "$checkout/build/debug/CMakeCache.txt"
# An external alias to an owned cache is still an overlap.
ln -s "$checkout/dist/shared" "$scratch/cache-alias"
CPKT_DEPENDENCY_CACHE="$scratch/cache-alias" reject dist
# Similar path prefixes are not children, and dist-only cleanup leaves build caches.
mkdir -p "$checkout/dist-other/cache"
printf retained >"$checkout/dist-other/cache/retained"
CPKT_DEPENDENCY_CACHE="$checkout/dist-other/cache" CPKT_TOOLCHAIN_CACHE="$checkout/build/tools" bash "$checkout/scripts/clean.sh" dist
[[ ! -e "$checkout/dist" && -f "$checkout/build/retained" && -f "$checkout/.cache/retained" ]]
[[ -f "$checkout/dist-other/cache/retained" ]]
seed
mkdir -p "$checkout/build/devenv/state/mcp-everything"
printf retained >"$checkout/build/devenv/state/mcp-everything/retained"
if CAI_CLEAN_TEARDOWN_FAIL=1 bash "$checkout/scripts/clean.sh" all >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ $(cat "$CAI_CLEAN_SERVICE_LOG") == down ]]
[[ -f "$checkout/build/devenv/state/mcp-everything/retained" ]]
for directory in build dist .cache; do [[ -f "$checkout/$directory/retained" ]]; done
: >"$CAI_CLEAN_SERVICE_LOG"
CPKT_DEPENDENCY_CACHE="$scratch/shared" CPKT_TOOLCHAIN_CACHE="$scratch/shared" bash "$checkout/scripts/clean.sh" all
[[ $(cat "$CAI_CLEAN_SERVICE_LOG") == down ]]
for directory in build dist .cache; do [[ ! -e "$checkout/$directory" ]]; done
[[ $(cat "$scratch/shared/retained") == borrowed ]]
