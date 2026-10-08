#!/usr/bin/env bash
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/devenv-test.XXXXXX")
peer=
finish() { rc=$?; if [[ -n "$peer" ]]; then kill "$peer" 2>/dev/null || true; wait "$peer" 2>/dev/null || true; fi; rm -rf "$scratch"; exit "$rc"; }
trap finish EXIT
checkout="$scratch/checkout with spaces"
mkdir -p "$checkout/scripts" "$checkout/cmake" "$scratch/bin"
cp "$repo/scripts/"{devenv,run_mcp_everything_service,run_mcp_everything_live_test}.sh "$checkout/scripts/"
cp "$repo/cmake/render_devenv.cmake" "$checkout/cmake/"
cp "$repo/devenv.yaml.in" "$checkout/"
cp -r "$repo/devenv" "$checkout/"
cat >"$scratch/bin/podman" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\t' "$@" >>"$CAI_FAKE_ROOT/events"; printf '\n' >>"$CAI_FAKE_ROOT/events"
case "$1:${2:-}" in
  info:*) [[ ${CAI_FAKE_FAIL:-} != info ]] || exit 125; printf '%s\n' "${CAI_FAKE_ROOTLESS:-true}" ;;
  pod:exists) [[ ${CAI_FAKE_FAIL:-} != exists ]] || exit 125; [[ -f "$CAI_FAKE_ROOT/pods" ]] && grep -Fxq "$3" "$CAI_FAKE_ROOT/pods" ;;
  kube:play) sed -n 's/^  name: "\([^"]*\)"$/\1/p' "${!#}" >>"$CAI_FAKE_ROOT/pods" ;;
  kube:down)
    [[ ${CAI_FAKE_FAIL:-} != down ]] || exit 7
    pod=$(sed -n 's/^  name: "\([^"]*\)"$/\1/p' "${!#}")
    grep -Fvx "$pod" "$CAI_FAKE_ROOT/pods" >"$CAI_FAKE_ROOT/pods.next" || true
    mv "$CAI_FAKE_ROOT/pods.next" "$CAI_FAKE_ROOT/pods"
    if [[ ${CAI_FAKE_FAIL:-} == down-reported ]]; then printf 'Error: network cleanup failed\n' >&2; fi ;;
  image:exists) exit 1 ;;
  exec:*) for kind in state tmp; do for service in mcp-everything searxng inspector; do directory="$CAI_FAKE_CHECKOUT/build/devenv/$kind/$service"; if [[ -d "$directory" ]]; then printf owner >"$directory/.cai-owner-check"; fi; done; done ;;
esac
SH
cat >"$scratch/bin/id" <<'SH'
#!/bin/sh
case "$1" in -u|-g) printf '1000\n' ;; esac
SH
chmod +x "$scratch/bin/"*
export PATH="$scratch/bin:$PATH" CAI_FAKE_ROOT="$scratch" CAI_FAKE_CHECKOUT="$checkout"
controller="$checkout/scripts/devenv.sh"
[[ $(bash "$controller" endpoint mcp-everything) == http://127.0.0.1:*/mcp ]]
[[ ! -e "$checkout/build" ]]
# Validate the entire selection and opt-in before any engine call or generated state.
for action in 'unknown' 'up mcp-everything unknown' 'up mcp-everything inspector' 'exec inspector'; do
  : >"$scratch/events"
  read -r -a args <<<"$action"
  if CAI_MCP_INSPECTOR_E2E=0 bash "$controller" "${args[@]}" >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
  [[ ! -s "$scratch/events" && ! -e "$checkout/build" ]]
done
bash "$controller" render mcp-everything
manifest="$checkout/build/devenv/mcp-everything.yaml"
grep -q 'kind: Pod' "$manifest"; grep -q 'keep-id' "$manifest"
[[ $(stat -c %a "$manifest") == 600 ]]
[[ $(stat -c %a "$checkout/build/devenv/credentials/searxng-secret") == 600 ]]
for failure in info exists; do
  if CAI_FAKE_FAIL="$failure" bash "$controller" exists mcp-everything >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
done
if CAI_FAKE_ROOTLESS=false bash "$controller" check >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
rm "$checkout/build/devenv/mcp-everything.settings"
if CAI_MCP_EVERYTHING_PORT=12 bash "$controller" endpoint mcp-everything >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
# Failure classification is critical for ownership: an engine error grants no pod ownership.
rm -f "$scratch/events"
if CAI_FAKE_FAIL=exists bash "$checkout/scripts/run_mcp_everything_service.sh" true >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
! grep -q $'kube\tplay' "$scratch/events"
# External endpoints bypass lifecycle ownership and are passed to the supplied command.
cat >"$scratch/scripts-test" <<'SH'
#!/bin/sh
printf '%s\n' "$CAI_MCP_EVERYTHING_BASE_URL" >"$CAI_FAKE_ROOT/endpoint"
SH
chmod +x "$scratch/scripts-test"
# Waiting protocol correctness is covered by HTTP fixture data in the controller test.
if bash "$checkout/scripts/run_mcp_everything_live_test.sh" /missing >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
mkdir "$scratch/peer"
printf '%s\n' '{"plan":[],"mcp_ready":true,"mcp_sse":true}' >"$scratch/peer/config.json"
"${CAI_TEST_PYTHON:-python3}" "$repo/tests/fixtures/http_peer.py" "$scratch/peer" >"$scratch/peer/out" 2>"$scratch/peer/err" & peer=$!
for n in {1..100}; do [[ ! -s "$scratch/peer/port" ]] || break; sleep .01; done
[[ -s "$scratch/peer/port" ]]
export CAI_MCP_EVERYTHING_BASE_URL="http://127.0.0.1:$(cat "$scratch/peer/port")/mcp"
bash "$controller" up mcp-everything
[[ $(stat -c %u "$checkout/build/devenv/state/mcp-everything/.cai-owner-check") == 1000 ]]
# A live pod never grants permission to synthesize a lost teardown manifest.
cp "$checkout/build/devenv/mcp-everything.settings" "$scratch/settings"
cp "$scratch/pods" "$scratch/retained-pods"
mv "$manifest" "$scratch/live.yaml"
: >"$scratch/events"
if bash "$controller" up mcp-everything >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
grep -q 'cannot render live pod' "$scratch/err"
[[ ! -e "$manifest" ]]
cmp "$scratch/settings" "$checkout/build/devenv/mcp-everything.settings"
cmp "$scratch/retained-pods" "$scratch/pods"
! grep -q $'kube\t' "$scratch/events"
mv "$scratch/live.yaml" "$manifest"
# Exec's command arguments are data, not additional service selectors.
bash "$controller" exec mcp-everything node -e 'fixture argument with spaces'
grep -q $'node\t-e\tfixture argument with spaces\t' "$scratch/events"
for failure in down down-reported; do
  if CAI_FAKE_FAIL="$failure" bash "$controller" down mcp-everything >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  [[ -f "$manifest" ]]
  # The first failed teardown leaves a live pod; the reported-error variant removes it.
done
# A scoped reset must never erase another service's teardown manifest or state.
bash "$controller" render searxng
searxng_manifest="$checkout/build/devenv/searxng.yaml"
sed -n 's/^  name: "\([^"]*\)"$/\1/p' "$searxng_manifest" >>"$scratch/pods"
printf retained >"$checkout/build/devenv/state/searxng/retained"
: >"$scratch/events"
if bash "$controller" reset mcp-everything >"$scratch/out" 2>"$scratch/err"; then exit 1; else [[ $? == 2 ]]; fi
grep -q 'reset stops all services' "$scratch/err"
[[ ! -s "$scratch/events" && -f "$searxng_manifest" ]]
[[ $(cat "$checkout/build/devenv/state/searxng/retained") == retained ]]
# Missing manifests and failed teardown retain all state and prevent reset.
mv "$searxng_manifest" "$scratch/retained.yaml"
if bash "$controller" reset >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
grep -q 'without its retained manifest' "$scratch/err"
[[ -f "$checkout/build/devenv/state/searxng/retained" ]]
mv "$scratch/retained.yaml" "$searxng_manifest"
if CAI_FAKE_FAIL=down bash "$controller" reset >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
[[ -f "$searxng_manifest" && -f "$checkout/build/devenv/state/searxng/retained" ]]
bash "$controller" reset
[[ ! -e "$checkout/build/devenv" ]]
# External readiness creates no pods; the supplied command receives its endpoint.
bash "$checkout/scripts/run_mcp_everything_service.sh" "$scratch/scripts-test"
[[ $(cat "$scratch/endpoint") == "$CAI_MCP_EVERYTHING_BASE_URL" ]]
printf '%s\n' '{"plan":[],"mcp_ready":false}' >"$scratch/peer/config.json"
if CAI_DEV_READY_TIMEOUT=1 bash "$controller" wait mcp-everything >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
grep -q 'readiness timed out' "$scratch/err"
