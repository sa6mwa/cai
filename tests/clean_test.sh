#!/usr/bin/env bash
# Local generated state is disposable, even when cache overrides name it.
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/clean-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
checkout="$scratch/checkout with spaces"
mkdir -p "$checkout/scripts" "$scratch/shared"
cp "$repo/scripts/clean.sh" "$checkout/scripts/"
cat >"$checkout/scripts/devenv.sh" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$CAI_CLEAN_SERVICE_LOG"
[[ ${CAI_CLEAN_TEARDOWN_FAIL:-0} != 1 ]]
SH
chmod +x "$checkout/scripts/devenv.sh"
export CAI_CLEAN_SERVICE_LOG="$scratch/services"
printf shared >"$scratch/shared/retained"
seed() {
  mkdir -p "$checkout/build/devenv" "$checkout/dist" "$checkout/.cache/tools"
  for directory in build dist .cache; do
    printf owned >"$checkout/$directory/retained"
    printf hidden >"$checkout/$directory/.hidden"
  done
}
seed
bash "$checkout/scripts/clean.sh" dist
[[ ! -e "$checkout/dist" && -f "$checkout/build/.hidden" && -f "$checkout/.cache/.hidden" ]]
seed
if CAI_CLEAN_TEARDOWN_FAIL=1 bash "$checkout/scripts/clean.sh" all; then exit 1; fi
for directory in build dist .cache; do [[ -f "$checkout/$directory/retained" ]]; done
# No cache name, stale CMake entry or external alias makes local state survive.
ln -s "$checkout/.cache/tools" "$scratch/local-alias"
printf 'CPKT_BOOTLIN_ROOT:PATH=%s\n' "$checkout/.cache/tools" >"$checkout/build/CMakeCache.txt"
CPKT_TOOLCHAIN_CACHE="$scratch/local-alias" CPKT_DEPENDENCY_CACHE="$checkout/.cache/deps" bash "$checkout/scripts/clean.sh" all
for directory in build dist .cache; do [[ ! -e "$checkout/$directory" ]]; done
[[ -f "$scratch/shared/retained" ]]
# Remove redirected entries themselves without deleting another owner's data
# or using its service manifest for teardown.
ln -s "$scratch/shared" "$checkout/build"
ln -s "$scratch/shared" "$checkout/.cache"
ln -s "$scratch/shared" "$checkout/dist"
: >"$CAI_CLEAN_SERVICE_LOG"
bash "$checkout/scripts/clean.sh" all
[[ ! -s "$CAI_CLEAN_SERVICE_LOG" && -f "$scratch/shared/retained" ]]
for directory in build dist .cache; do [[ ! -L "$checkout/$directory" ]]; done
