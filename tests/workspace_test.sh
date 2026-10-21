#!/usr/bin/env bash
set -Eeuo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/workspace.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "workspace fixture failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; cat "$scratch/out" "$scratch/err" >&2 2>/dev/null || true' ERR
mkdir -p "$scratch/owner/build" "$scratch/owner/.cache/deps" "$scratch/borrowed/.cache/deps" "$scratch/bin" "$scratch/global"
cat >"$scratch/bin/git" <<'SH'
#!/usr/bin/env bash
# Fixture metadata: both the owner and borrowed checkout belong to Git.
[[ "$2" == *borrowed* || "$2" == *owner* ]]
SH
chmod +x "$scratch/bin/git"
export PATH="$scratch/bin:$PATH"
cat >"$scratch/check.cmake" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
set(CMAKE_CURRENT_SOURCE_DIR "${SOURCE}")
set(CMAKE_CURRENT_BINARY_DIR "${BINARY}")
include("${POLICY}")
CMAKE
run() { cmake "-DPOLICY=$repo/cmake/CaiWorkspace.cmake" "-DSOURCE=$scratch/owner" "-DBINARY=$scratch/owner/build" "-DCPKT_DEPENDENCY_CACHE=$scratch/global" "$@" -P "$scratch/check.cmake" >"$scratch/out" 2>"$scratch/err"; }
run
run "-DCPKT_DEPENDENCY_CACHE=$scratch/owner/build/archives"
run "-DCPKT_DEPENDENCY_CACHE=$scratch/owner/.cache/archives"
run -DCPKT_DEPENDENCY_CACHE=build/relative-archives
ln -s "$scratch/borrowed/.cache" "$scratch/owner/build/borrowed-cache"
for args in "-DCAI_DEPS_DIR=$scratch/borrowed/.cache/deps" "-DBINARY=$scratch/borrowed/build" "-DCAI_DEPS_DIR=$scratch/owner/retained" "-DCPKT_DEPENDENCY_CACHE=$scratch/borrowed/.cache/deps" "-DCPKT_DEPENDENCY_CACHE=$scratch/owner/retained" "-DCPKT_DEPENDENCY_CACHE=$scratch/owner/build/borrowed-cache/archives" "-DCAI_DEPS_DIR=$scratch/owner/build/borrowed-cache/not-created" "-DBINARY=$scratch/owner/build/borrowed-cache/not-created"; do
  if run "$args"; then exit 1; fi
  grep -Eq 'borrowed|global archive cache' "$scratch/err"
done
mv "$scratch/owner/.cache" "$scratch/owner/cache-save"
ln -s "$scratch/borrowed/.cache" "$scratch/owner/.cache"
if run; then exit 1; fi
grep -Fq 'redirected' "$scratch/err"
# Prove the Darwin fixture's argv/profile routing with data-only host shims.
cat >"$scratch/bin/git" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$CAI_FAKE_GIT_ROOT"
SH
cat >"$scratch/bin/uname" <<'SH'
#!/bin/sh
printf '%s\n' Darwin
SH
cat >"$scratch/bin/sandbox-exec" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$@" >"$CAI_SANDBOX_ARGS"
SH
chmod +x "$scratch/bin/"*
export CAI_FAKE_GIT_ROOT='/work/repo with spaces' CAI_SANDBOX_ARGS="$scratch/sandbox-args"
bash "$repo/tests/run_without_repository.sh" "$scratch/owner" /bin/true --only smith_profile
[[ $(sed -n '1p' "$CAI_SANDBOX_ARGS") == -p ]]
[[ $(sed -n '2p' "$CAI_SANDBOX_ARGS") == '(version 1)(allow default)(deny file-read-metadata (literal "/work/repo with spaces/.git"))' ]]
[[ $(sed -n '3p' "$CAI_SANDBOX_ARGS") == /bin/true && $(sed -n '5p' "$CAI_SANDBOX_ARGS") == smith_profile ]]
