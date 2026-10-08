#!/usr/bin/env bash
# One rootless Podman Kube surface; no Python controller or hidden daemon.
set -euo pipefail
umask 077
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
state="$root/build/devenv"
action=${1:-check}; shift "$(( $# > 0 ? 1 : 0 ))"
services=("$@")
if [[ ${#services[@]} == 0 ]]; then
  services=(mcp-everything searxng)
  case "$action" in down|reset|ps|logs) services+=(inspector) ;; esac
fi
fail() { printf 'devenv: %s\n' "$*" >&2; exit 2; }
[[ ! -L "$root/build" && ! -L "$state" ]] || fail 'generated service root must not be a symlink'
checkout=$(printf %s "$root" | sha256sum | cut -c1-10)
base=$((30000 + 16#${checkout:0:7} % 9000))
select_service() {
  service=$1
  case "$service" in
    mcp-everything) prefix=CAI_MCP_EVERYTHING; default_image="localhost/cai-mcp-everything:${CAI_MCP_EVERYTHING_VERSION:-2026.1.26}"; offset=0 ;;
    searxng) prefix=CAI_SEARXNG; default_image=docker.io/searxng/searxng:2026.9.18-c0042add3; offset=1 ;;
    inspector) prefix=CAI_MCP_INSPECTOR; default_image=ghcr.io/modelcontextprotocol/inspector:1.0.1; offset=2 ;;
    *) fail "unknown service: $service" ;;
  esac
  pod="cai-$checkout-$service"; manifest="$state/$service.yaml"
  name="${prefix}_PORT"; port=${!name:-$((base+offset))}
  name="${prefix}_IMAGE"; image=${!name:-$default_image}
  if [[ -f "$state/$service.settings" ]]; then
    read -r saved_port <"$state/$service.settings"
    saved_image=$(sed -n '2p' "$state/$service.settings")
    port=$saved_port; image=$saved_image
  fi
  [[ "$port" =~ ^[0-9]+$ && "$port" -ge 1024 && "$port" -le 65535 ]] || fail 'service port must be 1024..65535'
  [[ "$image" =~ ^[A-Za-z0-9./_:-]+(:[A-Za-z0-9_.-]+|@sha256:[0-9a-f]{64})$ && "$image" != *:latest ]] || fail 'service image needs an exact tag or digest'
  url="http://127.0.0.1:$port"; [[ "$service" != mcp-everything ]] || url+=/mcp
  name="${prefix}_BASE_URL"; url=${!name:-$url}
}
exists() {
  local rc=0
  podman pod exists "$pod" || rc=$?
  [[ "$rc" -lt 2 ]] || fail "cannot inspect pod $pod (status $rc)"
  return "$rc"
}
require_podman() {
  command -v podman >/dev/null || fail 'install Podman and configure user namespaces'
  [[ $(id -u) != 0 && $(podman info --format '{{.Host.Security.Rootless}}') == true ]] || fail 'run rootless Podman as your ordinary user'
}
render() {
  if [[ -f "$manifest" ]] && exists; then
    name="${prefix}_PORT"; [[ -z ${!name:-} || ${!name} == "$port" ]] || fail 'stop the pod before changing its port'
    name="${prefix}_IMAGE"; [[ -z ${!name:-} || ${!name} == "$image" ]] || fail 'stop the pod before changing its image'
    return
  fi
  rm -f "$state/$service.settings"
  select_service "$service"
  mkdir -p "$state/state/$service" "$state/tmp/$service" "$state/credentials" "$state/logs"
  chmod 700 "$state/credentials"
  if [[ ! -f "$state/credentials/searxng-secret" ]]; then
    (umask 077; openssl rand -hex 32 >"$state/credentials/searxng-secret")
  fi
  cmake "-DROOT=$root" "-DOUTPUT=$manifest" "-DSERVICE=$service" "-DPOD=$pod" "-DIMAGE=$image" "-DPORT=$port" "-DURL=$url" "-DUID=$(id -u)" "-DGID=$(id -g)" "-DSECRET=${CAI_SEARXNG_SECRET:-$(cat "$state/credentials/searxng-secret")}" -P "$root/cmake/render_devenv.cmake"
  (umask 077; printf '%s\n%s\n' "$port" "$image" >"$state/$service.settings")
}
wait_ready() {
  local limit=${CAI_DEV_READY_TIMEOUT:-60} began=$SECONDS
  [[ "$limit" =~ ^[0-9]+$ && "$limit" -gt 0 && "$limit" -le 600 ]] || fail 'readiness timeout must be 1..600 seconds'
  mkdir -p "$state/logs"
  while ((SECONDS-began < limit)); do
    case "$service" in
      inspector) if podman exec "$pod-inspector" node --version >"$state/logs/$service-ready.log" 2>&1; then return; fi ;;
      searxng) if curl --noproxy '*' -fsS --max-time 2 "$url/" >"$state/logs/$service-ready.log" 2>&1; then return; fi ;;
      mcp-everything)
        if curl --noproxy '*' -fsS --max-time 2 -H 'content-type: application/json' -H 'accept: application/json, text/event-stream' \
          -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cai-kube-wait","version":"0.0.0"}}}' "$url" >"$state/logs/$service-ready.log" 2>&1; then
          if { sed -n 's/^data: *//p' "$state/logs/$service-ready.log"; if ! grep -q '^data:' "$state/logs/$service-ready.log"; then cat "$state/logs/$service-ready.log"; fi; } | jq -se 'any(.[]; .result.serverInfo|type == "object")' >/dev/null 2>&1; then return; fi
        fi ;;
    esac
    sleep .2
  done
  podman pod logs --tail 80 "$pod" >&2 || true
  fail "readiness timed out: $service ($url); $state/logs/$service-ready.log"
}
down() {
  if [[ -f "$manifest" ]] && exists; then
    mkdir -p "$state/logs"
    podman kube down "$manifest" >"$state/logs/$service-down.log" 2>&1 || {
      cat "$state/logs/$service-down.log" >&2
      fail "teardown failed; retained $manifest"
    }
    if grep -q '^Error:' "$state/logs/$service-down.log"; then
      cat "$state/logs/$service-down.log" >&2
      fail "Podman reported a teardown error; retained $manifest"
    fi
    if exists; then fail "pod survived teardown; retained $manifest"; fi
  fi
}
case "$action" in endpoint) for selected in "${services[@]}"; do select_service "$selected"; printf '%s\n' "$url"; done; exit ;; wait)
  # External loopback services don't require a Podman engine.
  for selected in "${services[@]}"; do select_service "$selected"; [[ "$selected" != inspector ]] || require_podman; wait_ready; done; exit ;;
esac
require_podman
created=()
cleanup_created() {
  rc=$?; trap - EXIT
  if [[ "$rc" != 0 && ${CAI_E2E_KEEP_DEVSERVICES:-0} != 1 ]]; then
    for selected in "${created[@]}"; do select_service "$selected"; down || true; done
  fi
  exit "$rc"
}
trap cleanup_created EXIT
for selected in "${services[@]}"; do
  select_service "$selected"
  if [[ "$service" == inspector && "$action" =~ ^(up|render|pull|exec)$ ]]; then [[ ${CAI_MCP_INSPECTOR_E2E:-0} == 1 ]] || fail 'inspector needs CAI_MCP_INSPECTOR_E2E=1'; fi
  case "$action" in
    check) ;;
    render) render ;;
    up)
      render
      if ! exists; then
        if [[ "$service" == mcp-everything && "$image" == localhost/cai-mcp-everything:* ]]; then
          rc=0; podman image exists "$image" || rc=$?
          [[ "$rc" -lt 2 ]] || fail 'cannot inspect MCP image'
          if [[ "$rc" == 1 ]]; then podman build --tag "$image" --build-arg "MCP_EVERYTHING_VERSION=${image##*:}" --file "$root/devenv/mcp-everything/Containerfile" "$root/devenv/mcp-everything"; fi
        fi
        created+=("$service")
        network=pasta; [[ "$service" != inspector ]] || network="pasta:-T,$port"
        podman kube play --network "$network" "$manifest"
      fi
      wait_ready
      if [[ "$service" == searxng ]]; then
        podman exec "$pod-$service" /usr/local/searxng/.venv/bin/python -c 'import pathlib; pathlib.Path("/var/cache/searxng/.cai-owner-check").write_text("owner"); pathlib.Path("/tmp/.cai-owner-check").write_text("owner")'
      else
        podman exec "$pod-$service" node -e 'for (const p of ["/state/.cai-owner-check","/tmp/.cai-owner-check"]) require("fs").writeFileSync(p,"owner")'
      fi
      for directory in state tmp; do [[ $(stat -c %u "$state/$directory/$service/.cai-owner-check") == "$(id -u)" ]] || fail 'service state has the wrong host owner'; done
      printf '%s_BASE_URL=%s pod=%s image=%s\n' "$prefix" "$url" "$pod" "$image" ;;
    down|reset) down ;;
    exists) exists || exit 1 ;;
    ps) podman pod ps --filter "name=$pod" ;;
    logs|diagnostics) if exists; then podman pod logs --tail 80 "$pod"; fi ;;
    pull) podman pull "$image" ;;
    exec) shift; podman exec "$pod-$service" "$@"; exit ;;
    *) fail 'use check/render/up/down/reset/ps/logs/exists/endpoint/wait/pull/exec' ;;
  esac
done
if [[ "$action" == reset ]]; then rm -rf "$state"; fi
