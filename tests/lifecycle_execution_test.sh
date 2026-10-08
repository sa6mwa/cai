#!/usr/bin/env bash
# Exercise sequencing with recorded commands, not real builds or release refs.
set -euo pipefail
repo=$1
native_make=$(command -v make)
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/lifecycle-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "lifecycle regression failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; cat "$scratch/events" "$scratch/err" >&2 2>/dev/null || true' ERR
mkdir -p "$scratch/scripts" "$scratch/bin"
cp "$repo/scripts/"{build,test,release,package}.sh "$scratch/scripts/"
cp "$repo/Makefile" "$scratch/"
cat >"$scratch/bin/record" <<'SH'
#!/usr/bin/env bash
printf '%s\t' "$@" >>"$CAI_LIFECYCLE_LOG"; printf '\n' >>"$CAI_LIFECYCLE_LOG"
if [[ ${CAI_LIFECYCLE_FAIL:-} == "$1" && ( -z ${CAI_LIFECYCLE_FAIL_ARG:-} || ${CAI_LIFECYCLE_FAIL_ARG} == "${2:-}" ) ]]; then exit 7; fi
SH
chmod +x "$scratch/bin/record"
for command in cmake ctest make; do printf '#!/usr/bin/env bash\nexec "%s/bin/record" %s "$@"\n' "$scratch" "$command" >"$scratch/bin/$command"; chmod +x "$scratch/bin/$command"; done
cat >"$scratch/bin/uname" <<'SH'
#!/usr/bin/env bash
case "$1" in -s) printf '%s\n' "${CAI_LIFECYCLE_SYSTEM:-Linux}" ;; -m) printf 'x86_64\n' ;; *) exit 2 ;; esac
SH
chmod +x "$scratch/bin/uname"
for script in clean.sh test_release_from_source.sh package-verify.sh run_mcp_everything_service.sh fuzz.sh; do
  printf '#!/usr/bin/env bash\nexec "%s/bin/record" %s "$@"\n' "$scratch" "$script" >"$scratch/scripts/$script"; chmod +x "$scratch/scripts/$script"
done
cat >"$scratch/scripts/release_version.sh" <<'SH'
#!/usr/bin/env bash
printf 'identity\t%s\n' "$*" >>"$CAI_LIFECYCLE_LOG"
[[ ${CAI_LIFECYCLE_FAIL:-} != identity ]] || exit 7
printf '1.2.3\n'
SH
printf '#!/bin/sh\nexit 1\n' >"$scratch/scripts/osxcross_available.sh"
cat >"$scratch/scripts/release_targets.sh" <<'SH'
#!/usr/bin/env bash
printf targets >>"$CAI_LIFECYCLE_TARGET_LOG"
printf 'x86_64-linux-gnu\n'
[[ ${CAI_LIFECYCLE_FAIL:-} != targets ]] || exit 7
SH
printf '#!/bin/sh\nprintf "http://127.0.0.1:3001/mcp\\n"\n' >"$scratch/scripts/devenv.sh"
chmod +x "$scratch/scripts/"*.sh
cat >"$scratch/scripts/cpkt-toolchains.sh" <<'SH'
#!/usr/bin/env bash
printf '%s %s\n' "$@" >>"$CAI_LIFECYCLE_PREP_LOG"
[[ ${CAI_LIFECYCLE_FAIL:-} != tools ]] || exit 7
SH
cat >"$scratch/scripts/cpkt-aflpp.sh" <<'SH'
#!/usr/bin/env bash
printf 'afl %s\n' "$*" >>"$CAI_LIFECYCLE_PREP_LOG"
[[ ${CAI_LIFECYCLE_FAIL:-} != tools ]] || exit 7
SH
chmod +x "$scratch/scripts/"cpkt-*.sh
export PATH="$scratch/bin:$PATH" CAI_LIFECYCLE_LOG="$scratch/events" CAI_LIFECYCLE_TARGET_LOG="$scratch/targets" CAI_LIFECYCLE_PREP_LOG="$scratch/preparation"
export CAI_ENABLE_INTEGRATION_TESTS=0 CAI_MCP_INSPECTOR_E2E=0
# Invalid actions/extra arguments do not provision targets or start producers.
for arguments in 'unknown' 'prepare unexpected'; do
  : >"$CAI_LIFECYCLE_LOG"; : >"$CAI_LIFECYCLE_TARGET_LOG"
  read -r -a argv <<<"$arguments"
  if bash "$scratch/scripts/release.sh" "${argv[@]}" >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
  [[ ! -s "$CAI_LIFECYCLE_LOG" && ! -s "$CAI_LIFECYCLE_TARGET_LOG" ]]
done
for mode in debug debug-cai cai release integration; do
  : >"$CAI_LIFECYCLE_LOG"
  bash "$scratch/scripts/test.sh" "$mode"
  [[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
  grep -q $'ctest\t--preset\t'"$mode" "$CAI_LIFECYCLE_LOG"
  grep -q -- --no-tests=error "$CAI_LIFECYCLE_LOG"
done
# Test failure propagates directly; configure failure never starts compilation.
: >"$CAI_LIFECYCLE_LOG"
if CAI_LIFECYCLE_FAIL=ctest bash "$scratch/scripts/test.sh" debug >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 7 ]]; fi
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
for argument in --preset --build; do
  : >"$CAI_LIFECYCLE_LOG"
  if CAI_LIFECYCLE_FAIL=cmake CAI_LIFECYCLE_FAIL_ARG="$argument" bash "$scratch/scripts/build.sh" debug >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 7 ]]; fi
  [[ $(tail -1 "$CAI_LIFECYCLE_LOG" | cut -f2) == "$argument" ]]
done
: >"$CAI_LIFECYCLE_LOG"
: >"$CAI_LIFECYCLE_TARGET_LOG"
export CAI_LIFECYCLE_FAIL=identity
if bash "$scratch/scripts/release.sh" final >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
grep -q '^identity' "$CAI_LIFECYCLE_LOG"
[[ ! -s "$CAI_LIFECYCLE_TARGET_LOG" ]]
# Producer failures stop the operation, with no package/clean/retry path.
for failed in clean.sh cmake make test_release_from_source.sh; do
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
# Preparation runs the opt-in live gate exactly once; offline preparation omits it.
mkdir -p "$scratch/build/x86_64-linux-gnu-release"
printf 'CMAKE_STRIP:FILEPATH=/target/strip\n' >"$scratch/build/x86_64-linux-gnu-release/CMakeCache.txt"
for enabled in 0 1; do
  : >"$CAI_LIFECYCLE_LOG"
  CAI_ENABLE_INTEGRATION_TESTS="$enabled" bash "$scratch/scripts/release.sh" prepare >"$scratch/out" 2>"$scratch/err"
  [[ $(grep -c $'^make\ttest-integration\t' "$CAI_LIFECYCLE_LOG" || true) == "$enabled" ]]
  [[ $(grep -c $'^make\ttest-all\t' "$CAI_LIFECYCLE_LOG") == 1 ]]
  ! grep -q '^clean.sh' "$CAI_LIFECYCLE_LOG"
done
# A failed live gate or final-byte verifier stops delivery, without retries.
for failed in make package-verify.sh; do
  : >"$CAI_LIFECYCLE_LOG"
  argument=; [[ "$failed" != make ]] || argument=test-integration
  if CAI_ENABLE_INTEGRATION_TESTS=1 CAI_LIFECYCLE_FAIL="$failed" CAI_LIFECYCLE_FAIL_ARG="$argument" bash "$scratch/scripts/release.sh" prepare >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 7 ]]; fi
  [[ $(tail -1 "$CAI_LIFECYCLE_LOG" | cut -f1) == "$failed" ]]
  ! grep -q 'Verified local artifact inventory' "$scratch/out"
done
# Exercise actual public Make recipes. Recursive leaves are recorded, not built.
run_make() {
  "$native_make" --no-print-directory -C "$scratch" RELEASE_VERSION=1.2.3 MAKE="$scratch/bin/make" "$@" >"$scratch/out" 2>"$scratch/err"
}
for target in test-lua-smith-e2e mcp-inspector-e2e prerelease-hardening; do
  : >"$CAI_LIFECYCLE_LOG"
  if run_make "$target"; then exit 1; fi
  [[ ! -s "$CAI_LIFECYCLE_LOG" ]]
  grep -q 'Set CAI_' "$scratch/err"
done
: >"$CAI_LIFECYCLE_LOG"
CAI_ENABLE_INTEGRATION_TESTS=1 run_make test-lua-smith-e2e
[[ $(head -1 "$CAI_LIFECYCLE_LOG") == $'make\tlua-rock\tintegration-build\t' ]]
grep -q 'cai_lua_smith_e2e' "$CAI_LIFECYCLE_LOG"
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 2 ]]
: >"$CAI_LIFECYCLE_LOG"
CAI_MCP_INSPECTOR_E2E=1 run_make mcp-inspector-e2e
grep -q -- $'--target\tcai_mcp_http_server\t' "$CAI_LIFECYCLE_LOG"
grep -q 'cai_mcp_inspector_e2e' "$CAI_LIFECYCLE_LOG"
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 3 ]]
: >"$CAI_LIFECYCLE_LOG"
if CAI_MCP_INSPECTOR_E2E=1 CAI_LIFECYCLE_FAIL=cmake run_make mcp-inspector-e2e; then exit 1; fi
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
for target in test-e2e mcp-everything-test; do
  : >"$CAI_LIFECYCLE_LOG"
  # The latter executes the fixture directly instead of the service wrapper.
  mkdir -p "$scratch/build/debug"
  printf '#!/bin/sh\nexec "%s/bin/record" mcp-everything "$@"\n' "$scratch" >"$scratch/build/debug/cai_mcp_everything_e2e"
  chmod +x "$scratch/build/debug/cai_mcp_everything_e2e"
  run_make "$target" CAI_MCP_EVERYTHING_BASE_URL=http://127.0.0.1:3001/mcp
  grep -q -- $'--target\tcai_mcp_everything_e2e\t' "$CAI_LIFECYCLE_LOG"
  [[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 3 ]]
done
: >"$CAI_LIFECYCLE_LOG"
CAI_ENABLE_INTEGRATION_TESTS=1 run_make prerelease-hardening
[[ $(head -1 "$CAI_LIFECYCLE_LOG") == $'make\tprerelease\t' ]]
[[ $(tail -1 "$CAI_LIFECYCLE_LOG") == $'fuzz.sh\tlong\t' ]]
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 2 ]]
for target in test-lua-smith-e2e prerelease-hardening; do
  : >"$CAI_LIFECYCLE_LOG"
  if CAI_ENABLE_INTEGRATION_TESTS=1 CAI_LIFECYCLE_FAIL=make run_make "$target"; then exit 1; fi
  [[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
done
# Discovery errors cannot become an empty/partial checksum inventory.
: >"$CAI_LIFECYCLE_LOG"
if CAI_LIFECYCLE_FAIL=targets run_make package-checksums; then exit 1; fi
[[ ! -s "$CAI_LIFECYCLE_LOG" ]]
# Preparation belongs before configure, selects one collection and stops on failure.
for mode in debug cai aarch64-linux-musl-release fuzz; do
  : >"$CAI_LIFECYCLE_LOG"; : >"$CAI_LIFECYCLE_PREP_LOG"
  bash "$scratch/scripts/build.sh" "configure-$mode"
  [[ $(wc -l <"$CAI_LIFECYCLE_PREP_LOG") == 1 ]]
  case "$mode" in
    debug) expected='ensure x86_64-linux-gnu' ;;
    cai) expected='ensure x86_64-linux-musl' ;;
    fuzz) expected='afl ensure' ;;
    *) expected='ensure aarch64-linux-musl' ;;
  esac
  [[ $(cat "$CAI_LIFECYCLE_PREP_LOG") == "$expected" ]]
  [[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
  : >"$CAI_LIFECYCLE_LOG"
  if CAI_LIFECYCLE_FAIL=tools bash "$scratch/scripts/build.sh" "configure-$mode" >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 7 ]]; fi
  [[ ! -s "$CAI_LIFECYCLE_LOG" ]]
done
: >"$CAI_LIFECYCLE_PREP_LOG"
CAI_LIFECYCLE_SYSTEM=Darwin bash "$scratch/scripts/build.sh" configure-debug
[[ ! -s "$CAI_LIFECYCLE_PREP_LOG" ]]
: >"$CAI_LIFECYCLE_LOG"
run_make package-checksums
grep -q -- '-DCAI_TARGETS=x86_64-linux-gnu' "$CAI_LIFECYCLE_LOG"
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
for failed in identity targets; do
  : >"$CAI_LIFECYCLE_LOG"; : >"$CAI_LIFECYCLE_TARGET_LOG"
  if CAI_LIFECYCLE_FAIL="$failed" bash "$scratch/scripts/package.sh" >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 7 ]]; fi
  [[ $(cat "$CAI_LIFECYCLE_LOG") == identity$'\t'* ]]
  [[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 1 ]]
  [[ "$failed" != identity || ! -s "$CAI_LIFECYCLE_TARGET_LOG" ]]
done
: >"$CAI_LIFECYCLE_LOG"
bash "$scratch/scripts/package.sh"
[[ $(head -1 "$CAI_LIFECYCLE_LOG") == identity$'\t'* ]]
grep -q -- '-DCAI_VERSION=1.2.3' "$CAI_LIFECYCLE_LOG"
[[ $(wc -l <"$CAI_LIFECYCLE_LOG") == 3 ]]
: >"$CAI_LIFECYCLE_LOG"
if bash "$scratch/scripts/package.sh" release-matrix unexpected >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
[[ ! -s "$CAI_LIFECYCLE_LOG" ]]
