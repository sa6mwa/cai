#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
makefile=$repo_root/Makefile
compose_file=$repo_root/docker-compose.yaml
readme=$repo_root/README.md

if [ ! -f "$makefile" ]; then
  printf 'Makefile not found: %s\n' "$makefile" >&2
  exit 1
fi
if [ ! -f "$readme" ]; then
  printf 'README not found: %s\n' "$readme" >&2
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
  deps-debug deps-release deps-cross build-host cross-build chatgpt-login test-host \
  test-cross cross-test test-all test-e2e test-install-tree coverage test-coverage valgrind fuzz-long \
  verify-release-archives verify-release-privacy require-prerelease-live \
  require-clean-worktree lifecycle-version-contract release-pipeline dev-up dev-down dev-reset dev-ps dev-logs \
  mcp-inspector-e2e clean-dist clangd-check; do
  require_target "$target"
done

for target in \
  deps-debug deps-release deps-cross build-debug build-host build-release \
  cross-build cross-test integration-build chatgpt-login test-debug test-host test-release \
  test-all test-e2e test-cross test-integration test-install-tree asan \
  test-asan coverage test-coverage valgrind fuzz-long verify-release-archives \
  verify-release-privacy require-prerelease-live dev-up dev-down dev-reset \
  require-clean-worktree lifecycle-version-contract dev-ps dev-logs clean-dist \
  clangd-check searxng-pull searxng-up searxng-wait searxng-test \
  searxng-down searxng-logs mcp-everything-up mcp-everything-wait \
  mcp-everything-test mcp-everything-down mcp-everything-logs \
  mcp-everything-live-test mcp-inspector-e2e package package-source package-source-smoke \
  package-checksums package-verify release-matrix release format; do
  require_help "$target"
done

for script in \
  scripts/deps.sh scripts/test.sh scripts/host_test.sh \
  scripts/build.sh scripts/package.sh scripts/package-verify.sh \
  scripts/cross_build.sh scripts/cross_test.sh \
  scripts/clean.sh scripts/release_version.sh \
  scripts/run_linux_release_matrix.sh scripts/test_release_from_source.sh \
  scripts/verify_release_privacy.sh scripts/validate_luarocks.sh \
  scripts/osxcross_available.sh \
  scripts/cpkt-toolchains.sh scripts/cpkt-aflpp.sh \
  scripts/fuzz.sh scripts/run_valgrind.sh \
  scripts/compose.sh scripts/dev-up.sh scripts/dev-down.sh \
  scripts/dev-reset.sh scripts/dev-ps.sh scripts/dev-logs.sh \
  scripts/test-e2e.sh; do
  require_script "$script"
done

if ! grep -F 'CAI_C_PKT_SYSTEMS_VERSION ?= 0.9.0' "$makefile" >/dev/null; then
  printf 'Makefile must pin c.pkt.systems 0.9.0\n' >&2
  exit 1
fi
if ! grep -F 'CAI_LONEJSON_VERSION ?= 0.42.0' "$makefile" >/dev/null; then
  printf 'Makefile must pin lonejson 0.42.0\n' >&2
  exit 1
fi
if ! grep -F 'CAI_PSLOG_VERSION ?= 0.9.0' "$makefile" >/dev/null; then
  printf 'Makefile must pin libpslog 0.9.0\n' >&2
  exit 1
fi

if ! grep -F 'COMPOSE := bash ./scripts/compose.sh' "$makefile" >/dev/null; then
  printf 'Makefile compose operations must route through scripts/compose.sh\n' >&2
  exit 1
fi
for route in \
  'bash ./scripts/deps.sh debug' \
  'bash ./scripts/deps.sh release' \
  'bash ./scripts/build.sh debug' \
  'bash ./scripts/build.sh release-matrix' \
  'bash ./scripts/build.sh host' \
  'bash ./scripts/build.sh integration' \
  'bash ./scripts/build.sh coverage' \
  'bash ./scripts/build.sh asan' \
  'bash ./scripts/build.sh valgrind' \
  'bash ./scripts/build.sh fuzz' \
  'bash ./scripts/build.sh package-source' \
  'bash ./scripts/build.sh configure-debug' \
  'bash ./scripts/host_test.sh' \
  'bash ./scripts/cross_build.sh' \
  'bash ./scripts/cross_test.sh' \
  'bash ./scripts/test.sh debug $(CTEST_FLAGS)' \
  'bash ./scripts/test.sh release $(CTEST_FLAGS)' \
  'bash ./scripts/test.sh integration $(CTEST_FLAGS)' \
  'bash ./scripts/package.sh release-matrix' \
  'bash ./scripts/package-verify.sh' \
  'bash ./scripts/test_release_from_source.sh' \
  'bash ./scripts/clean.sh'; do
  if ! grep -F "$route" "$makefile" >/dev/null; then
    printf 'Makefile must route lifecycle behavior through standard script: %s\n' \
      "$route" >&2
    exit 1
  fi
done
if ! grep -F 'bash ./scripts/clean.sh dist' "$makefile" >/dev/null; then
  printf 'Makefile clean-dist must route through scripts/clean.sh dist\n' >&2
  exit 1
fi
if ! grep -F '"$repo_root/scripts/build.sh" package-source' \
  "$repo_root/scripts/package.sh" >/dev/null; then
  printf 'package.sh must route source archive generation through guarded build.sh package-source\n' >&2
  exit 1
fi
for live_target in \
  test-integration prerelease-live \
  prerelease-hardening mcp-everything-live-test mcp-inspector-e2e; do
  if ! grep -F "make $live_target" "$makefile" |
       grep -E 'CAI_ENABLE_INTEGRATION_TESTS=1|CAI_MCP_INSPECTOR_E2E=1' >/dev/null; then
    printf 'make help must list the required opt-in variable for live/e2e target: %s\n' \
      "$live_target" >&2
    exit 1
  fi
done
if ! grep -F 'CAI_MCP_INSPECTOR_IMAGE ?= ghcr.io/modelcontextprotocol/inspector:1.0.1' \
  "$makefile" >/dev/null; then
  printf 'Makefile must pin the default MCP Inspector image tag\n' >&2
  exit 1
fi
if grep -R -n 'ghcr.io/modelcontextprotocol/inspector:[l]atest' \
  "$repo_root/README.md" "$repo_root/examples" "$repo_root/tests/mcp_inspector_e2e.sh" \
  "$repo_root/docker-compose.yaml" >/dev/null; then
  printf 'MCP Inspector e2e must not default to a floating latest image tag\n' >&2
  exit 1
fi
if grep -F './scripts/detect_release_version.sh' "$makefile" >/dev/null; then
  printf 'Makefile must use scripts/release_version.sh for release version resolution\n' >&2
  exit 1
fi
if ! grep -F 'exec "$script_dir/release_version.sh" "$@"' \
  "$repo_root/scripts/detect_release_version.sh" >/dev/null; then
  printf 'detect_release_version.sh must be only a compatibility wrapper around release_version.sh\n' >&2
  exit 1
fi
if grep -F './scripts/test_release_source.sh' "$makefile" >/dev/null; then
  printf 'Makefile must use scripts/test_release_from_source.sh for source archive smoke tests\n' >&2
  exit 1
fi
if git -C "$repo_root" ls-files 'stash/*' | grep . >/dev/null; then
  printf 'tracked top-level stash files must not be release source inputs\n' >&2
  exit 1
fi
if grep -R -n 'stash/' "$repo_root/README.md" "$repo_root/docs" |
     grep -v 'lifecycle-migration.md:' >/dev/null; then
  printf 'README/docs must not point at top-level stash paths\n' >&2
  exit 1
fi
if ! grep -F 'scripts/osxcross_available.sh' "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'scripts/osxcross_available.sh' "$repo_root/scripts/package.sh" >/dev/null; then
  printf 'build/package release matrices must use scripts/osxcross_available.sh\n' >&2
  exit 1
fi
if ! grep -F 'configure_preset debug "$native_toolchain_file" "$(native_target_id)"' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset integration "$native_toolchain_file" "$(native_target_id)"' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset coverage "$native_toolchain_file" "$(native_target_id)"' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset release "$bootlin_toolchain_file" x86_64-linux-gnu' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset asan "$bootlin_toolchain_file" x86_64-linux-gnu' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset valgrind "$bootlin_toolchain_file" x86_64-linux-gnu' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset fuzz "$aflpp_toolchain_file" x86_64-linux-gnu 0' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'configure_preset x86_64-linux-gnu-release "$bootlin_toolchain_file" x86_64-linux-gnu' \
     "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'native_target_id' "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'linux-aflpp.cmake' "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'native-lifecycle.cmake' "$repo_root/CMakePresets.json" >/dev/null ||
   ! grep -F 'CMAKE_TOOLCHAIN_FILE' "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'CPKT_BOOTLIN_ROOT' "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F 'CMAKE_C_COMPILER' "$repo_root/scripts/build.sh" >/dev/null ||
   ! grep -F -- '--fresh' "$repo_root/scripts/build.sh" >/dev/null; then
  printf 'build.sh must refresh stale native Linux builds onto the pinned Bootlin toolchain\n' >&2
  exit 1
fi
for route in \
  'bash ./scripts/build.sh configure-debug' \
  'bash ./scripts/build.sh configure-release' \
  'bash ./scripts/build.sh configure-integration'; do
  if ! grep -F "$route" "$repo_root/scripts/deps.sh" >/dev/null; then
    printf 'deps.sh must route configure behavior through guarded build.sh mode: %s\n' \
      "$route" >&2
    exit 1
  fi
done
if ! grep -F '$(MAKE) build-debug' "$repo_root/examples/Makefile" >/dev/null; then
  printf 'examples Makefile must route debug configuration through guarded build-debug\n' >&2
  exit 1
fi
if ! grep -F 'CAI_ENABLE_INTEGRATION_TESTS=1 make test-integration' \
  "$readme" >/dev/null; then
  printf 'README must document the standard opt-in test-integration command\n' >&2
  exit 1
fi
if ! grep -F 'CAI_ENABLE_INTEGRATION_TESTS=1 make prerelease-live' \
  "$readme" >/dev/null; then
  printf 'README must document the standard opt-in prerelease-live command\n' >&2
  exit 1
fi
if grep -F 'cmake --preset debug -DCAI_BUILD_INTEGRATION_TESTS=ON' \
  "$readme" >/dev/null; then
  printf 'README must not document the old debug-preset integration command\n' >&2
  exit 1
fi
if ! grep -F 'CAI_MCP_INSPECTOR_E2E=1 make mcp-inspector-e2e' \
  "$readme" "$repo_root/examples/mcp-server/README.md" >/dev/null; then
  printf 'README docs must route MCP Inspector e2e through make mcp-inspector-e2e\n' >&2
  exit 1
fi
if grep -R -n 'CAI_MCP_INSPECTOR_E2E=1 ctest --preset' \
  "$readme" "$repo_root/examples" >/dev/null; then
  printf 'MCP Inspector docs must not route the e2e workflow through raw ctest\n' >&2
  exit 1
fi
if ! grep -F 'make asan' "$readme" >/dev/null ||
   grep -F 'cmake --build --preset asan' "$readme" >/dev/null; then
  printf 'README must route the ASan developer check through make asan\n' >&2
  exit 1
fi

if ! grep -F '$(MAKE) test-e2e' "$makefile" >/dev/null; then
  printf 'prerelease must include deterministic compose-backed test-e2e\n' >&2
  exit 1
fi
if ! grep -F '$(MAKE) valgrind' "$makefile" >/dev/null; then
  printf 'prerelease must include the native Valgrind gate\n' >&2
  exit 1
fi
test_all_body=$(awk '
  /^test-all:/ {
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
if ! printf '%s\n' "$test_all_body" | grep -F '$(MAKE) test-release' >/dev/null; then
  printf 'test-all must include native release unit tests\n' >&2
  exit 1
fi
test_install_tree_body=$(awk '
  /^test-install-tree:/ {
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
if ! printf '%s\n' "$test_install_tree_body" |
     grep -F "cai_install_metadata_test" >/dev/null; then
  printf 'test-install-tree must run the installed SDK metadata/consumer test\n' >&2
  exit 1
fi
if grep -Eq '^tsan:|^msan:|CMAKE_C_COMPILER.*clang|libFuzzer|fuzz-full' "$makefile"; then
  printf 'Makefile must not retain Clang compiler, TSan, MSan, or libFuzzer paths\n' >&2
  exit 1
fi

clangd_body=$(awk '
  /^clangd-check:/ {
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
if ! printf '%s\n' "$clangd_body" | grep -F 'build/debug' >/dev/null ||
   ! printf '%s\n' "$clangd_body" | grep -F 'cmake/clangd_check.cmake' >/dev/null; then
  printf '%s\n' 'clangd-check must validate only the native debug compile database' >&2
  exit 1
fi
if ! grep -F -- '--tweaks=' "$repo_root/cmake/clangd_check.cmake" >/dev/null; then
  printf '%s\n' 'clangd check must disable unsupported clangd developer tweaks' >&2
  exit 1
fi

hardening_body=$(awk '
  /^prerelease-hardening:/ {
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
if ! printf '%s\n' "$hardening_body" | grep -F '$(MAKE) fuzz-long' >/dev/null; then
  printf 'prerelease-hardening must run the long AFL++ fuzz target\n' >&2
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
if ! printf '%s\n' "$prerelease_live_body" | grep -F 'CAI_ENABLE_INTEGRATION_TESTS:-' >/dev/null ||
   ! printf '%s\n' "$prerelease_live_body" | grep -F '$(MAKE) test-integration' >/dev/null; then
  printf 'prerelease-live must require caller opt-in and run full live integration tests\n' >&2
  exit 1
fi
if printf '%s\n' "$prerelease_live_body" | grep -F 'CAI_ENABLE_INTEGRATION_TESTS=1 $(MAKE)' >/dev/null; then
  printf 'prerelease-live must not manufacture the live integration opt-in\n' >&2
  exit 1
fi
if ! printf '%s\n' "$prerelease_live_body" | grep -F 'rm -f "$(RELEASE_LIVE_GATE_STAMP)"' >/dev/null; then
  printf 'prerelease-live must invalidate the live gate stamp before live checks\n' >&2
  exit 1
fi

release_pipeline_body=$(awk '
  /^release-pipeline:/ {
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
for command in \
  '$(MAKE) test-debug' \
  '$(MAKE) valgrind' \
  '$(MAKE) fuzz-smoke' \
  '$(MAKE) lua-test' \
  '$(MAKE) test-e2e' \
  '$(MAKE) release-matrix'; do
  if ! printf '%s\n' "$release_pipeline_body" | grep -F "$command" >/dev/null; then
    printf 'release-pipeline is missing required command: %s\n' "$command" >&2
    exit 1
  fi
done
if printf '%s\n' "$release_pipeline_body" | grep -F 'clangd-check' >/dev/null; then
  printf '%s\n' 'release-pipeline must not invoke the host-only clangd gate' >&2
  exit 1
fi
if [ "$(printf '%s\n' "$release_pipeline_body" | grep -nF '$(MAKE) release-matrix' | cut -d: -f1)" -le "$(printf '%s\n' "$release_pipeline_body" | grep -nF '$(MAKE) test-debug' | cut -d: -f1)" ]; then
  printf '%s\n' 'release-pipeline must run ordinary tests before release-matrix' >&2
  exit 1
fi

release_matrix_body=$(awk '
  /^release-matrix:/ {
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
if ! printf '%s\n' "$release_matrix_body" | grep -F '$(MAKE) package-source-smoke' >/dev/null; then
  printf '%s\n' 'release-matrix must smoke-test the generated source archive' >&2
  exit 1
fi

prerelease_line=$(awk '/^prerelease:/ { print; exit }' "$makefile")
case " $prerelease_line " in
  *" release-pipeline "*)
    ;;
  *)
  printf '%s\n' 'prerelease must invoke the shared release-pipeline' >&2
  exit 1
    ;;
esac
if ! printf '%s\n' "$prerelease_live_body" | grep -F '$(MAKE) require-clean-worktree' >/dev/null; then
  printf 'prerelease-live must require a clean worktree before stamping\n' >&2
  exit 1
fi
if printf '%s\n' "$prerelease_live_body" | grep -F '$(MAKE) example-smoke-live' >/dev/null; then
  printf 'prerelease-live must not run API-key live example smoke tests\n' >&2
  exit 1
fi

mcp_everything_live_body=$(awk '
  /^mcp-everything-live-test:/ {
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
if ! printf '%s\n' "$mcp_everything_live_body" | grep -F 'CAI_ENABLE_INTEGRATION_TESTS' >/dev/null; then
  printf 'mcp-everything-live-test must require explicit live integration opt-in\n' >&2
  exit 1
fi
if ! grep -F 'scripts/run_mcp_everything_live_test.sh' "$repo_root/CMakeLists.txt" >/dev/null; then
  printf 'live MCP client integration test must own MCP Everything service startup\n' >&2
  exit 1
fi
if ! printf '%s\n' "$prerelease_live_body" | grep -F '$(RELEASE_LIVE_GATE_STAMP)' >/dev/null; then
  printf 'prerelease-live must write the release live gate stamp\n' >&2
  exit 1
fi

mcp_inspector_body=$(awk '
  /^mcp-inspector-e2e:/ {
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
if ! printf '%s\n' "$mcp_inspector_body" | grep -F 'CAI_MCP_INSPECTOR_E2E' >/dev/null ||
   ! printf '%s\n' "$mcp_inspector_body" | grep -F '$(MAKE) build-debug' >/dev/null ||
   ! printf '%s\n' "$mcp_inspector_body" | grep -F 'cai_mcp_inspector_e2e' >/dev/null; then
  printf 'mcp-inspector-e2e must guard opt-in, build debug, and run the focused CTest\n' >&2
  exit 1
fi
if [ "$(printf '%s\n' "$mcp_inspector_body" | grep -nF 'CAI_MCP_INSPECTOR_E2E' | head -1 | cut -d: -f1)" -ge "$(printf '%s\n' "$mcp_inspector_body" | grep -nF '$(MAKE) build-debug' | head -1 | cut -d: -f1)" ]; then
  printf 'mcp-inspector-e2e must check CAI_MCP_INSPECTOR_E2E before building\n' >&2
  exit 1
fi
if ! awk '
  /set_tests_properties\(cai_mcp_inspector_e2e PROPERTIES/ {
    in_test = 1
    next
  }
  in_test && /TIMEOUT 180/ {
    found = 1
  }
  in_test && /\)/ {
    exit
  }
  END {
    exit(found ? 0 : 1)
  }
' "$repo_root/CMakeLists.txt"; then
  printf 'cai_mcp_inspector_e2e must allow a bounded cold container image pull\n' >&2
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
    printf 'release must not depend on prerelease-live; live verification is an explicit separate gate: %s\n' \
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
release_first_command=$(printf '%s\n' "$release_body" | sed -n '/^[[:space:]]*[^[:space:]#]/ { s/^[[:space:]]*//; p; q; }')
release_second_command=$(printf '%s\n' "$release_body" | sed -n '/^[[:space:]]*[^[:space:]#]/ { n; s/^[[:space:]]*//; p; q; }')
if [ "$release_first_command" != '$(MAKE) lifecycle-version-contract' ]; then
  printf 'release must run lifecycle-version-contract first: %s\n' \
    "$release_first_command" >&2
  exit 1
fi
if [ "$release_second_command" != '$(MAKE) clean' ]; then
  printf 'release must clean immediately after lifecycle-version-contract: %s\n' \
    "$release_second_command" >&2
  exit 1
fi
release_third_command=$(printf '%s\n' "$release_body" | sed -n '/^[[:space:]]*[^[:space:]#]/ { n; n; s/^[[:space:]]*//; p; q; }')
if [ "$release_third_command" != '$(MAKE) release-pipeline' ]; then
  printf 'release must invoke the shared release-pipeline after clean: %s\n' \
    "$release_third_command" >&2
  exit 1
fi

lifecycle_version_contract_body=$(awk '
  /^lifecycle-version-contract:/ {
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
if ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F "reserved_tag='v99.99.99'" >/dev/null; then
  printf '%s\n' 'lifecycle-version-contract must reserve the standard temporary release tag' >&2
  exit 1
fi
if ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F 'cleanup() { git tag -d "$$reserved_tag"' >/dev/null ||
   ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F 'trap cleanup EXIT HUP INT TERM' >/dev/null; then
  printf '%s\n' 'lifecycle-version-contract must clean the temporary release tag' >&2
  exit 1
fi
if ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F 'git -c tag.gpgSign=false tag "$$reserved_tag"' >/dev/null; then
  printf '%s\n' 'lifecycle-version-contract must create the temporary tag unsigned' >&2
  exit 1
fi
if ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F 'untagged_version="$$(./scripts/release_version.sh' >/dev/null ||
   ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F "expected 0.0.0" >/dev/null; then
  printf '%s\n' 'lifecycle-version-contract must assert untagged release rehearsals resolve to 0.0.0 before temporary tagging' >&2
  exit 1
fi
if ! printf '%s\n' "$lifecycle_version_contract_body" | grep -F 'tag_type="$$(git cat-file -t "refs/tags/$$exact_tag"' >/dev/null; then
  printf '%s\n' 'lifecycle-version-contract must verify exact tags are lightweight' >&2
  exit 1
fi

release_pipeline_body=$(awk '
  /^release-pipeline:/ {
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
release_pipeline_first_command=$(printf '%s\n' "$release_pipeline_body" | sed -n '/^[[:space:]]*[^[:space:]#]/ { s/^[[:space:]]*//; p; q; }')
release_pipeline_second_command=$(printf '%s\n' "$release_pipeline_body" | sed -n '/^[[:space:]]*[^[:space:]#]/ { n; s/^[[:space:]]*//; p; q; }')
if [ "$release_pipeline_first_command" != '$(MAKE) format' ]; then
  printf 'release-pipeline must run format first: %s\n' \
    "$release_pipeline_first_command" >&2
  exit 1
fi
if [ "$release_pipeline_second_command" != '$(MAKE) require-clean-worktree' ]; then
  printf 'release-pipeline must fail on formatting edits before tests/packages: %s\n' \
    "$release_pipeline_second_command" >&2
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
