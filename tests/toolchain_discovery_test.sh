#!/usr/bin/env bash
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/toolchain-discovery.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/scripts"
cp "$repo/scripts/"{osxcross_available,release_targets}.sh "$scratch/scripts/"
cat >"$scratch/scripts/cpkt-toolchains.sh" <<'SH'
#!/usr/bin/env bash
printf 'cache=/fixture/status=ready\nstatus=%s\n' "$CAI_DISCOVERY_STATUS"
exit "${CAI_DISCOVERY_EXIT:-0}"
SH
chmod +x "$scratch/scripts/cpkt-toolchains.sh"
export CAI_DISCOVERY_STATUS=missing CAI_DISCOVERY_EXIT=0
available="$scratch/scripts/osxcross_available.sh"
targets="$scratch/scripts/release_targets.sh"
if bash "$available" --quiet; then exit 1; else [[ $? == 1 ]]; fi
[[ $(bash "$targets" | wc -l) == 6 ]]
[[ $(CAI_DISCOVERY_STATUS=ready bash "$targets" | wc -l) == 7 ]]
for status in unknown ''; do
  if CAI_DISCOVERY_STATUS="$status" bash "$targets" >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
  grep -q 'invalid toolchain status' "$scratch/err"
done
if CAI_DISCOVERY_EXIT=7 bash "$targets" >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
grep -q 'toolchain discovery failed' "$scratch/err"
if bash "$available" --quiet unexpected >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
