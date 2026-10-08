#!/usr/bin/env bash
# Exercise sequencing with recorded commands, not real builds or release refs.
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/lifecycle-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/scripts" "$scratch/bin"
cp "$repo/scripts/"{build,test,release}.sh "$scratch/scripts/"
cat >"$scratch/bin/record" <<'SH'
#!/usr/bin/env bash
printf '%s\t' "$@" >>"$CAI_LIFECYCLE_LOG"; printf '\n' >>"$CAI_LIFECYCLE_LOG"
[[ ${CAI_LIFECYCLE_FAIL:-} != "$1" ]] || exit 7
SH
chmod +x "$scratch/bin/record"
for command in cmake ctest make; do printf '#!/usr/bin/env bash\nexec "%s/bin/record" %s "$@"\n' "$scratch" "$command" >"$scratch/bin/$command"; chmod +x "$scratch/bin/$command"; done
for script in clean.sh test_release_from_source.sh package-verify.sh run_mcp_everything_service.sh; do
  printf '#!/usr/bin/env bash\nexec "%s/bin/record" %s "$@"\n' "$scratch" "$script" >"$scratch/scripts/$script"; chmod +x "$scratch/scripts/$script"
done
cat >"$scratch/scripts/release_version.sh" <<'SH'
#!/usr/bin/env bash
printf 'identity\t%s\n' "$*" >>"$CAI_LIFECYCLE_LOG"
[[ ${CAI_LIFECYCLE_FAIL:-} != identity ]] || exit 7
printf '1.2.3\n'
SH
printf '#!/bin/sh\nexit 1\n' >"$scratch/scripts/osxcross_available.sh"
printf '#!/bin/sh\nprintf "x86_64-linux-gnu\\n"\n' >"$scratch/scripts/release_targets.sh"
printf '#!/bin/sh\nprintf "http://127.0.0.1:3001/mcp\\n"\n' >"$scratch/scripts/devenv.sh"
chmod +x "$scratch/scripts/"*.sh
export PATH="$scratch/bin:$PATH" CAI_LIFECYCLE_LOG="$scratch/events"
for mode in debug debug-cai cai release integration; do
  : >"$CAI_LIFECYCLE_LOG"
  bash "$scratch/scripts/test.sh" "$mode"
  [[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
  grep -q $'ctest\t--preset\t'"$mode" "$CAI_LIFECYCLE_LOG"
  grep -q -- --no-tests=error "$CAI_LIFECYCLE_LOG"
done
: >"$CAI_LIFECYCLE_LOG"
export CAI_LIFECYCLE_FAIL=identity
if bash "$scratch/scripts/release.sh" final >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
grep -q '^identity' "$CAI_LIFECYCLE_LOG"
# Producer failures stop the operation, with no package/clean/retry path.
for failed in cmake make test_release_from_source.sh; do
  : >"$CAI_LIFECYCLE_LOG"; export CAI_LIFECYCLE_FAIL="$failed"
  if bash "$scratch/scripts/release.sh" final >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  [[ $(head -1 "$CAI_LIFECYCLE_LOG" | cut -f1) == identity ]]
  [[ $(sed -n '2p' "$CAI_LIFECYCLE_LOG" | cut -f1) == clean.sh ]]
  [[ $(grep -c '^clean.sh' "$CAI_LIFECYCLE_LOG") == 1 ]]
  [[ $(tail -1 "$CAI_LIFECYCLE_LOG" | cut -f1) == "$failed" ]]
  ! grep -q '^package-verify.sh' "$CAI_LIFECYCLE_LOG"
done
# Positive final sequencing uses one native source lane and its exact cache.
unset CAI_LIFECYCLE_FAIL
cat >"$scratch/scripts/test_release_from_source.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
"$(dirname "$0")/../bin/record" test_release_from_source.sh "$@"
mkdir -p "$1/build/release-source-smoke/build"
printf 'CMAKE_STRIP:FILEPATH=/target/strip\n' >"$1/build/release-source-smoke/build/CMakeCache.txt"
SH
chmod +x "$scratch/scripts/test_release_from_source.sh"
: >"$CAI_LIFECYCLE_LOG"
bash "$scratch/scripts/release.sh" final >"$scratch/out" 2>"$scratch/err"
[[ $(grep -c '^identity' "$CAI_LIFECYCLE_LOG") == 1 ]]
[[ $(grep -c '^clean.sh' "$CAI_LIFECYCLE_LOG") == 1 ]]
[[ $(grep -c '^test_release_from_source.sh' "$CAI_LIFECYCLE_LOG") == 1 ]]
[[ $(grep -c '^package-verify.sh' "$CAI_LIFECYCLE_LOG") == 1 ]]
! grep -q -- $'--preset\tx86_64-linux-gnu-release' "$CAI_LIFECYCLE_LOG"
grep -q 'release-source-smoke/build' "$CAI_LIFECYCLE_LOG"
