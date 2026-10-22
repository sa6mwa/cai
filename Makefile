SHELL := bash
.SHELLFLAGS := -e -o pipefail -c
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules
.NOTPARALLEL:
ROOT := $(CURDIR)
ifneq ($(realpath $(firstword $(MAKEFILE_LIST))),$(CURDIR)/Makefile)
$(error run make -C $(dir $(firstword $(MAKEFILE_LIST))) so the checkout owns generated paths)
endif
CMAKE := cmake
CTEST := ctest
CTEST_FLAGS ?=
CTEST_FILTER ?= -L unit -LE 'live|packaging'
DEVENV := bash ./scripts/devenv.sh
RELEASE_VERSION ?= $(shell ./scripts/release_version.sh "$(ROOT)")
ifeq ($(strip $(RELEASE_VERSION)),)
$(error invalid release version; run scripts/release_version.sh)
endif
CAI_SEARXNG_BASE_URL ?= $(shell $(DEVENV) endpoint searxng)
CAI_SEARXNG_TEST_ENGINE ?= wikipedia
CAI_SEARXNG_TEST_QUERY ?= OpenAI
CAI_MCP_EVERYTHING_BASE_URL ?= $(shell $(DEVENV) endpoint mcp-everything)
CAI_MCP_INSPECTOR_IMAGE ?= ghcr.io/modelcontextprotocol/inspector:1.0.1
CAI_CPKT_VERSION ?= 0.5.0
CAI_LONEJSON_VERSION ?= 0.45.0
CAI_PSLOG_VERSION ?= 0.12.0
LONEJSON_LUA_ROCK_URL ?= https://github.com/sa6mwa/lonejson/releases/download/v$(CAI_LONEJSON_VERSION)/lonejson-$(CAI_LONEJSON_VERSION)-1.src.rock
PSLOG_LUA_ROCK_URL ?= https://github.com/sa6mwa/libpslog/releases/download/v$(CAI_PSLOG_VERSION)/lua-pslog-$(CAI_PSLOG_VERSION)-1.src.rock
LUA_ROCK_TREE := build/luarocks
LUA_ROCKSPEC := $(LUA_ROCK_TREE)/cai-$(RELEASE_VERSION)-1.rockspec
LUA_ROCK_EXTRA_CFLAGS ?= -O3 -DNDEBUG
LUA_ROCK_PREFIX := $(LUA_ROCK_TREE)/cai-prefix
LUA_LONEJSON_ROCK_STAMP := $(LUA_ROCK_TREE)/lib/luarocks/rocks-5.5/lonejson/$(CAI_LONEJSON_VERSION)-1/rock_manifest
LUA_PSLOG_ROCK_STAMP := $(LUA_ROCK_TREE)/lib/luarocks/rocks-5.5/lua-pslog/$(CAI_PSLOG_VERSION)-1/rock_manifest

.PHONY: help build build-debug build-cli build-host build-release deps-debug deps-release deps-cross cross-build cross-test test-cross test test-debug test-cli test-host test-release test-all test-e2e test-offline test-integration integration-build test-install-tree valgrind asan test-asan fuzz fuzz-smoke fuzz-long coverage test-coverage lua-rock lua-runner lua-env lua-test format format-check finalize-slice clangd-check prerelease prerelease-live prerelease-hardening release-pipeline release release-matrix lifecycle-version-contract require-clean-worktree package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-lua-artifacts print-release-version chatgpt-login test-lua-smith-e2e test-smith-goal-e2e example-smoke-local clean clean-dist devenv-check dev-up dev-down dev-reset dev-ps dev-logs searxng-pull searxng-up searxng-wait searxng-down searxng-logs searxng-test mcp-everything-up mcp-everything-wait mcp-everything-down mcp-everything-logs mcp-everything-test mcp-everything-live-test mcp-inspector-e2e
help:
	@printf '%s\n' 'make build / test       Native debug library and unit feedback.' 'make build-cli / test-cli Native CLI and fixtures; no cross builds.' 'make test-all           Native tests once per scope, Lua, memory, fuzz and local e2e.' 'make format / format-check Source formatting only; no configure or build.' 'make prerelease         Native development/hardening checks plus the artifact matrix.' 'make release-matrix     Incremental target builds, Release/CLI tests and artifact verification.' 'make release            Clean signed, tagged final artifact gate; no publish or ref mutation.' 'make package            Warm binary matrix packaging.' 'make package-source / package-source-smoke Explicit source shipment and reconstruction.' 'make package-verify     Verify existing final bytes, without rebuilding producers.' 'make valgrind / asan / fuzz-smoke / fuzz-long / test-coverage Hardening.' 'make lua-rock / lua-test / lua-env Native Lua facade and source rock.' 'make test-offline / test-integration Local fixtures / opt-in credentialed tests.' 'make dev-up / dev-down / dev-reset / dev-ps / dev-logs Rootless Podman Kube services.' 'make clean / clean-dist Remove owned generated state / dist only.' 'See README and scripts for focused installed-SDK, service and model checks.'

build: build-debug
build-debug:
	bash scripts/build.sh debug
build-cli:
	bash scripts/build.sh debug-cai
build-host:
	bash scripts/build.sh release
build-release:
	bash scripts/release.sh build-matrix
deps-debug:
	bash scripts/build.sh configure-debug
deps-release:
	bash scripts/build.sh configure-release
deps-cross:
	bash scripts/release.sh configure-matrix
cross-build: build-release
cross-test:
	bash scripts/cross_test.sh
test-cross: cross-test

test: test-debug
test-debug: build-debug
	bash scripts/test.sh debug $(CTEST_FLAGS) $(CTEST_FILTER)
test-cli: build-cli
	bash scripts/test.sh debug-cai $(CTEST_FLAGS)
test-host: test-release
test-release: build-host
	bash scripts/test.sh release $(CTEST_FLAGS) -L unit -LE 'live|host'
test-all:
	$(MAKE) test-debug CTEST_FILTER="-LE 'live|valgrind'"
	$(MAKE) test-cli
	$(MAKE) valgrind
	$(MAKE) fuzz-smoke
	$(MAKE) lua-test
	$(MAKE) lua-app-test
	$(MAKE) test-offline
	$(MAKE) test-e2e

test-e2e:
	bash scripts/build.sh debug cai_mcp_everything_e2e
	bash scripts/run_mcp_everything_service.sh "$(ROOT)/build/debug/cai_mcp_everything_e2e" "$(CAI_MCP_EVERYTHING_BASE_URL)"
integration-build: $(LUA_PSLOG_ROCK_STAMP) $(LUA_LONEJSON_ROCK_STAMP)
	bash scripts/build.sh integration
test-offline: $(LUA_PSLOG_ROCK_STAMP) $(LUA_LONEJSON_ROCK_STAMP)
	bash scripts/build.sh integration cai_integration_tests cai_lua cai_lua_runner
	bash scripts/test.sh integration $(CTEST_FLAGS) -L offline -R '^cai_(integration_|lua_chatgpt_auth_)'
test-integration:
	@test "$${CAI_ENABLE_INTEGRATION_TESTS:-0}" = 1 || { printf 'Set CAI_ENABLE_INTEGRATION_TESTS=1 for live checks.\n' >&2; exit 2; }
	$(MAKE) lua-rock integration-build
	bash scripts/test.sh integration $(CTEST_FLAGS) -L live
test-lua-smith-e2e:
	@test "$${CAI_ENABLE_INTEGRATION_TESTS:-0}" = 1 || { printf 'Set CAI_ENABLE_INTEGRATION_TESTS=1 for live checks.\n' >&2; exit 2; }
	$(MAKE) lua-rock integration-build
	bash scripts/test.sh integration $(CTEST_FLAGS) -R '^cai_lua_smith_e2e$$'
test-smith-goal-e2e:
	@test "$${CAI_ENABLE_INTEGRATION_TESTS:-0}" = 1
	$(MAKE) integration-build
	bash scripts/test.sh integration -R '^cai_integration_chatgpt_smith_goal_e2e$$'
test-install-tree: build-debug
	bash scripts/test.sh debug $(CTEST_FLAGS) -R '^cai_install_metadata_test$$'
valgrind:
	bash scripts/build.sh valgrind cai_tests
	bash scripts/test.sh valgrind $(CTEST_FLAGS)
asan:
	bash scripts/build.sh asan
	bash scripts/test.sh asan $(CTEST_FLAGS) -L unit -LE 'live|host|packaging'
test-asan: asan
fuzz: fuzz-smoke
fuzz-smoke:
	bash scripts/build.sh fuzz cai_fuzzers
	bash scripts/test.sh fuzz $(CTEST_FLAGS) -L fuzz
fuzz-long: fuzz-smoke
	bash scripts/fuzz.sh long
coverage:
	bash scripts/build.sh coverage
test-coverage: coverage
	bash scripts/test.sh coverage $(CTEST_FLAGS) -L unit -LE 'live|host|packaging'
example-smoke-local: build-debug
	bash scripts/test.sh debug -L example-smoke
chatgpt-login:
	$(MAKE) -C examples run-chatgpt-login
format:
	bash scripts/format.sh
format-check:
	bash scripts/format.sh check
finalize-slice:
	$(MAKE) format
	$(MAKE) test-debug
clangd-check: build-debug
	$(CMAKE) -DCAI_SOURCE_DIR="$(ROOT)" -DCAI_BUILD_DIR="$(ROOT)/build/debug" -P cmake/clangd_check.cmake
require-clean-worktree:
	@test -z "$$(git status --porcelain=v1 --untracked-files=all)" || { git status --short >&2; exit 1; }
lifecycle-version-contract:
	bash scripts/release_version.sh "$(ROOT)" --require-release
prerelease:
	bash scripts/release.sh prepare
release-pipeline: prerelease
prerelease-live: test-integration
prerelease-hardening:
	@test "$${CAI_ENABLE_INTEGRATION_TESTS:-0}" = 1 || { printf 'Set CAI_ENABLE_INTEGRATION_TESTS=1 for prerelease-hardening.\n' >&2; exit 2; }
	$(MAKE) prerelease
	# Preparation already passed live coverage and fuzz smoke; extend only fuzzing.
	bash scripts/fuzz.sh long
release:
	@test '$(origin RELEASE_VERSION)' = file || { printf 'Final release forbids RELEASE_VERSION overrides; use the exact signed, tagged commit.\n' >&2; exit 2; }
	bash scripts/release.sh final
release-matrix:
	bash scripts/release.sh matrix
package:
	bash scripts/package.sh release-matrix
package-source:
	$(CMAKE) -DCAI_ROOT="$(ROOT)" -DCAI_DIST_DIR="$(ROOT)/dist" -DCAI_VERSION="$(RELEASE_VERSION)" -P cmake/package_source.cmake
package-source-smoke: package-source
	bash scripts/test_release_from_source.sh "$(ROOT)" "$(ROOT)/dist/cai-$(RELEASE_VERSION).tar.gz"
package-checksums:
	targets="$$(bash scripts/release_targets.sh | paste -sd ';')"; \
	$(CMAKE) -DCAI_DIST_DIR="$(ROOT)/dist" -DCAI_VERSION="$(RELEASE_VERSION)" -DCAI_TARGETS="$$targets" -P cmake/package_checksums.cmake
package-verify:
	bash scripts/package-verify.sh "$(ROOT)" "$(RELEASE_VERSION)"
verify-release-archives: package-verify
verify-release-privacy: package-verify
print-release-version:
	@bash scripts/release_version.sh "$(ROOT)"
clean:
	bash scripts/clean.sh
clean-dist:
	bash scripts/clean.sh dist

$(LUA_ROCKSPEC): cai.rockspec.in scripts/render_release_rockspec.sh | deps-debug
	mkdir -p "$(LUA_ROCK_TREE)"
	lib_ext="$$(luarocks config variables.LIB_EXTENSION)"; ./scripts/render_release_rockspec.sh "$(RELEASE_VERSION)" "$(LUA_ROCKSPEC)" "git+file://$(CURDIR)" "" "$$lib_ext" ""

$(LUA_LONEJSON_ROCK_STAMP): | deps-debug
	bash scripts/lua_dependencies.sh lonejson "$(LONEJSON_LUA_ROCK_URL)"

$(LUA_PSLOG_ROCK_STAMP): | deps-debug
	bash scripts/lua_dependencies.sh pslog "$(PSLOG_LUA_ROCK_URL)"

lua-rock: $(LUA_ROCKSPEC) $(LUA_LONEJSON_ROCK_STAMP) $(LUA_PSLOG_ROCK_STAMP)
	$(CMAKE) --build --preset debug --target cai_static cai_shared
	$(CMAKE) --install build/debug --prefix "$(LUA_ROCK_PREFIX)"
	bash scripts/install_lua_rock.sh "$(LUA_ROCKSPEC)"

lua-runner: $(LUA_PSLOG_ROCK_STAMP) $(LUA_LONEJSON_ROCK_STAMP)
	$(CMAKE) --preset debug-lua
	$(CMAKE) --build --preset debug-lua --target cai_lua_runner

lua-env:
	printf '%s\n' 'eval "$$(luarocks path --tree "$(ROOT)/$(LUA_ROCK_TREE)")"'; \
	printf 'export CAI_LUA_EXECUTABLE="%s/build/debug-lua/cai_lua_runner"\n' "$(ROOT)"

lua-test: lua-runner
	$(CMAKE) --build --preset debug-lua --target cai_lua cai_lua_native_todo_store_test cai_lua_runner cai_mcp_http_server
	bash ./scripts/test.sh debug-lua -R '^cai_lua_' -LE live

.PHONY: lua-app-test
lua-app-test: $(LUA_PSLOG_ROCK_STAMP) $(LUA_LONEJSON_ROCK_STAMP)
	$(CMAKE) --preset debug-cai -DCAI_BUILD_LUA=ON
	$(CMAKE) --build --preset debug-cai --target cai_app_lua cai_lua_runner cai_app_host cai_terminal_peer
	bash scripts/test.sh debug-cai -R '^cai_app_lua' -LE live

release-lua-artifacts:
	bash scripts/package_lua.sh "$(RELEASE_VERSION)"

devenv-check:
	$(DEVENV) check
dev-up:
	$(DEVENV) up
dev-down:
	$(DEVENV) down
dev-reset:
	$(DEVENV) reset
dev-ps:
	$(DEVENV) ps
dev-logs:
	$(DEVENV) logs
searxng-test:
	curl -fsS --get "$(CAI_SEARXNG_BASE_URL)/search" --data-urlencode "q=$(CAI_SEARXNG_TEST_QUERY)" --data-urlencode format=json --data-urlencode "engines=$(CAI_SEARXNG_TEST_ENGINE)"
mcp-everything-test:
	bash scripts/build.sh debug cai_mcp_everything_e2e
	build/debug/cai_mcp_everything_e2e "$(CAI_MCP_EVERYTHING_BASE_URL)"
mcp-everything-live-test:
	@test "$${CAI_ENABLE_INTEGRATION_TESTS:-0}" = 1
	$(MAKE) integration-build
	bash scripts/test.sh integration -R '^cai_integration_chatgpt_mcp_client_tool$$'
mcp-inspector-e2e:
	@test "$${CAI_MCP_INSPECTOR_E2E:-0}" = 1 || { printf 'Set CAI_MCP_INSPECTOR_E2E=1 for inspector checks.\n' >&2; exit 2; }
	bash scripts/build.sh debug cai_mcp_http_server
	bash scripts/test.sh debug $(CTEST_FLAGS) -R '^cai_mcp_inspector_e2e$$'
searxng-pull:
	$(DEVENV) pull searxng
searxng-up:
	$(DEVENV) up searxng
searxng-wait:
	$(DEVENV) wait searxng
searxng-down:
	$(DEVENV) down searxng
searxng-logs:
	$(DEVENV) logs searxng
mcp-everything-up:
	$(DEVENV) up mcp-everything
mcp-everything-wait:
	$(DEVENV) wait mcp-everything
mcp-everything-down:
	$(DEVENV) down mcp-everything
mcp-everything-logs:
	$(DEVENV) logs mcp-everything
