#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
makefile=$repo_root/Makefile
compose_file=$repo_root/docker-compose.yaml

if [ ! -f "$makefile" ]; then
  printf 'Makefile not found: %s\n' "$makefile" >&2
  exit 1
fi

require_target() {
  target=$1
  if ! grep -E "^${target}:" "$makefile" >/dev/null; then
    printf 'required lifecycle Make target is missing: %s\n' "$target" >&2
    exit 1
  fi
}

require_help() {
  target=$1
  if ! grep -F "make $target" "$makefile" >/dev/null; then
    printf 'make help must list lifecycle target: %s\n' "$target" >&2
    exit 1
  fi
}

require_script() {
  script=$1
  if [ ! -x "$repo_root/$script" ]; then
    printf 'required lifecycle script is missing or not executable: %s\n' \
      "$script" >&2
    exit 1
  fi
}

for target in \
  deps-debug deps-release deps-cross build-host cross-build test-host \
  test-cross cross-test test-all test-e2e coverage test-coverage fuzz-long \
  verify-release-archives verify-release-privacy require-prerelease-live \
  require-clean-worktree dev-up dev-down dev-reset dev-ps dev-logs \
  clean-dist; do
  require_target "$target"
done

for target in \
  deps-debug deps-release deps-cross build-host cross-build test-all test-e2e \
  test-cross coverage test-coverage fuzz-long verify-release-archives \
  verify-release-privacy require-prerelease-live dev-up dev-down dev-reset \
  require-clean-worktree dev-ps dev-logs clean-dist; do
  require_help "$target"
done

for script in \
  scripts/compose.sh scripts/dev-up.sh scripts/dev-down.sh \
  scripts/dev-reset.sh scripts/dev-ps.sh scripts/dev-logs.sh \
  scripts/test-e2e.sh scripts/host_release_preset.sh; do
  require_script "$script"
done

if ! grep -F 'COMPOSE := bash ./scripts/compose.sh' "$makefile" >/dev/null; then
  printf 'Makefile compose operations must route through scripts/compose.sh\n' >&2
  exit 1
fi

if ! grep -F '$(MAKE) test-e2e' "$makefile" >/dev/null; then
  printf 'prerelease must include deterministic compose-backed test-e2e\n' >&2
  exit 1
fi

prerelease_live_body=$(awk '
  /^prerelease-live:/ {
    in_target = 1
    next
  }
  in_target && /^[^[:space:]].*:/ {
    exit
  }
  in_target {
    print
  }
' "$makefile")
if ! printf '%s\n' "$prerelease_live_body" | grep -F 'CAI_ENABLE_INTEGRATION_TESTS=1 $(MAKE) test-integration' >/dev/null; then
  printf 'prerelease-live must enable and run full live integration tests\n' >&2
  exit 1
fi
if ! printf '%s\n' "$prerelease_live_body" | grep -F '$(MAKE) require-clean-worktree' >/dev/null; then
  printf 'prerelease-live must require a clean worktree before stamping\n' >&2
  exit 1
fi
if ! printf '%s\n' "$prerelease_live_body" | grep -F 'CAI_ENABLE_INTEGRATION_TESTS=1 $(MAKE) example-smoke-live' >/dev/null; then
  printf 'prerelease-live must enable and run live example smoke tests\n' >&2
  exit 1
fi
if ! printf '%s\n' "$prerelease_live_body" | grep -F '$(RELEASE_LIVE_GATE_STAMP)' >/dev/null; then
  printf 'prerelease-live must write the release live gate stamp\n' >&2
  exit 1
fi

release_line=$(awk '
  /^release:/ {
    print
    found = 1
    exit
  }
  END {
    if (!found) {
      exit 1
    }
  }
' "$makefile") || {
  printf 'release target is missing\n' >&2
  exit 1
}

case " $release_line " in
  *" require-prerelease-live "*)
    ;;
  *)
    printf 'release must require a successful prerelease-live gate: %s\n' \
      "$release_line" >&2
    exit 1
    ;;
esac

release_body=$(awk '
  /^release:/ {
    in_target = 1
    next
  }
  in_target && /^[^[:space:]].*:/ {
    exit
  }
  in_target {
    print
  }
' "$makefile")
if ! printf '%s\n' "$release_body" | grep -F '$(MAKE) require-clean-worktree' >/dev/null; then
  printf 'release must re-check clean worktree before packaging artifacts\n' >&2
  exit 1
fi

build_host_body=$(awk '
  /^build-host:/ {
    in_target = 1
    next
  }
  in_target && /^[^[:space:]].*:/ {
    exit
  }
  in_target {
    print
  }
' "$makefile")
if ! printf '%s\n' "$build_host_body" | grep -F '$(HOST_RELEASE_PRESET)' >/dev/null; then
  printf 'build-host must resolve the explicit host release target preset\n' >&2
  exit 1
fi
if printf '%s\n' "$build_host_body" | grep -F ' --preset release' >/dev/null; then
  printf 'build-host must not use the generic release preset as a host target\n' >&2
  exit 1
fi

test_release_body=$(awk '
  /^test-release:/ {
    in_target = 1
    next
  }
  in_target && /^[^[:space:]].*:/ {
    exit
  }
  in_target {
    print
  }
' "$makefile")
if ! printf '%s\n' "$test_release_body" | grep -F '$(HOST_RELEASE_PRESET)' >/dev/null; then
  printf 'test-release must resolve the explicit host release target preset\n' >&2
  exit 1
fi
if printf '%s\n' "$test_release_body" | grep -F 'build/release' >/dev/null; then
  printf 'test-release must not use the generic release build tree as a host target\n' >&2
  exit 1
fi

if [ ! -f "$compose_file" ]; then
  printf 'docker-compose.yaml not found: %s\n' "$compose_file" >&2
  exit 1
fi
if ! grep -F 'name: cai-e2e' "$compose_file" >/dev/null; then
  printf 'docker-compose.yaml must set the lifecycle project name\n' >&2
  exit 1
fi
if grep -E '^[[:space:]]*container_name:' "$compose_file" >/dev/null; then
  printf 'docker-compose.yaml must not pin fixed container names\n' >&2
  exit 1
fi
if grep -E 'image:.*:latest' "$compose_file" >/dev/null; then
  printf 'docker-compose.yaml must not use latest image tags\n' >&2
  exit 1
fi

target_line=$(awk '
  /^mcp-everything-test:/ {
    print
    found = 1
    exit
  }
  END {
    if (!found) {
      exit 1
    }
  }
' "$makefile") || {
  printf 'mcp-everything-test target is missing\n' >&2
  exit 1
}

case " $target_line " in
  *" build-debug "*)
    ;;
  *)
    printf 'mcp-everything-test must depend on build-debug: %s\n' \
      "$target_line" >&2
    exit 1
    ;;
esac

if ! grep -F '$(CMAKE) --build build/debug --target cai_mcp_everything_e2e' \
  "$makefile" >/dev/null; then
  printf 'mcp-everything-test must build cai_mcp_everything_e2e\n' >&2
  exit 1
fi

wait_body=$(awk '
  /^mcp-everything-wait:/ {
    in_target = 1
    next
  }
  in_target && /^[^	][^:]*:/ {
    exit
  }
  in_target {
    print
  }
' "$makefile")

if printf '%s\n' "$wait_body" | grep -F '{1..30}' >/dev/null; then
  printf 'mcp-everything-wait must not use shell-specific brace expansion\n' >&2
  exit 1
fi

if ! printf '%s\n' "$wait_body" | grep -F 'while [ "$$attempt" -le 30 ]; do' \
  >/dev/null; then
  printf 'mcp-everything-wait must use a POSIX retry loop\n' >&2
  exit 1
fi
