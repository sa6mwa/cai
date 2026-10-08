#!/usr/bin/env bash
set -euo pipefail
repo=$1; scratch=$(mktemp -d "$2/acquisition.XXXXXX")
peer=
cleanup() { rc=$?; if [[ -n "$peer" ]]; then kill "$peer" 2>/dev/null || true; wait "$peer" 2>/dev/null || true; fi; rm -rf "$scratch"; exit "$rc"; }
trap cleanup EXIT
mkdir -p "$scratch/source/pkg" "$scratch/deps"
printf verified >"$scratch/source/pkg/marker"
tar -czf "$scratch/payload.tar.gz" -C "$scratch/source" pkg
digest=$(sha256sum "$scratch/payload.tar.gz" | cut -d' ' -f1)
"${CAI_TEST_PYTHON:-python3}" "$repo/tests/fixtures/archive_peer.py" "$scratch" & peer=$!
for n in {1..100}; do [[ ! -s "$scratch/port" ]] || break; sleep .01; done
[[ -s "$scratch/port" ]]
cat >"$scratch/acquire.cmake" <<'CMAKE'
include("${ROOT}/cmake/CaiAcquisition.cmake")
cai_dependency_archive_path("payload.tar.gz" "${DIGEST}" archive)
cai_download_tarball("fixture" "${URL}" "${archive}" "${CAI_DEPS_DIR}/pkg" "${DIGEST}" "${CAI_DEPS_DIR}/pkg/marker")
CMAKE
run() { cmake "-DROOT=$repo" "-DCPKT_DEPENDENCY_CACHE=$scratch/cache" "-DCAI_DEPS_DIR=$scratch/deps" "-DDIGEST=$digest" "-DURL=http://127.0.0.1:$(cat "$scratch/port")/payload.tar.gz" -P "$scratch/acquire.cmake"; }
run; [[ $(wc -l <"$scratch/requests") == 1 ]]; [[ $(cat "$scratch/deps/pkg/marker") == verified ]]
run; [[ $(wc -l <"$scratch/requests") == 1 ]]
archive="$scratch/cache/archives/sha256/$digest/payload.tar.gz"
printf corrupt >"$archive"; run; [[ $(wc -l <"$scratch/requests") == 2 ]]
rm "$archive"; printf corrupt >"$scratch/payload.tar.gz"
if run >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ ! -e "$archive" && ! -e "$archive.tmp" && $(wc -l <"$scratch/requests") == 3 ]]
grep -q 'SHA256 mismatch' "$scratch/err"
