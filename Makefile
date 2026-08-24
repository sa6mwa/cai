SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules

ROOT := $(CURDIR)
CMAKE := cmake
CTEST := ctest
CTEST_FLAGS := --stop-on-failure
COMPOSE_FILE := docker-compose.yaml
COMPOSE := bash ./scripts/compose.sh
CAI_SEARXNG_BASE_URL ?= http://127.0.0.1:8888
CAI_SEARXNG_TEST_ENGINE ?= wikipedia
CAI_SEARXNG_TEST_QUERY ?= OpenAI
CAI_MCP_EVERYTHING_BASE_URL ?= http://127.0.0.1:3001/mcp
CAI_MCP_INSPECTOR_IMAGE ?= ghcr.io/modelcontextprotocol/inspector:1.0.1
CAI_FUZZ_SECONDS ?= 10
CAI_FUZZ_LONG_SECONDS ?= 120
RELEASE_VERSION ?= $(shell ./scripts/release_version.sh "$(CURDIR)")
ifeq ($(strip $(RELEASE_VERSION)),)
$(error release version resolver returned empty; run ./scripts/release_version.sh "$(CURDIR)")
endif
CAI_CPKT_TARGET ?= x86_64-linux-gnu
CAI_C_PKT_SYSTEMS_VERSION ?= 0.9.0
CAI_LONEJSON_VERSION ?= 0.42.0
CAI_PSLOG_VERSION ?= 0.9.0
LONEJSON_LUA_ROCK_URL ?= https://github.com/sa6mwa/lonejson/releases/download/v$(CAI_LONEJSON_VERSION)/lonejson-$(CAI_LONEJSON_VERSION)-1.src.rock
PSLOG_LUA_ROCK_URL ?= https://github.com/sa6mwa/libpslog/releases/download/v$(CAI_PSLOG_VERSION)/lua-pslog-$(CAI_PSLOG_VERSION)-1.src.rock
CAI_C_PKT_SYSTEMS_PREFIX := $(CURDIR)/.cache/deps/c.pkt.systems-$(CAI_C_PKT_SYSTEMS_VERSION)-$(CAI_CPKT_TARGET)
CAI_LONEJSON_PREFIX := $(CURDIR)/.cache/deps/liblonejson-$(CAI_LONEJSON_VERSION)-$(CAI_CPKT_TARGET)
CAI_PSLOG_PREFIX := $(CURDIR)/.cache/deps/libpslog-$(CAI_PSLOG_VERSION)-$(CAI_CPKT_TARGET)
LUA_ROCK_TREE := build/luarocks
LUA_ROCKSPEC := $(LUA_ROCK_TREE)/cai-$(RELEASE_VERSION)-1.rockspec
LUA_ROCK_STAMP := $(LUA_ROCK_TREE)/.installed.stamp
LUA_ROCK_BUILD_LOCK := $(LUA_ROCK_TREE)/.build.lock
LUA_ROCK_EXTRA_CFLAGS ?= -O3 -DNDEBUG
LUA_ROCK_PREFIX := $(LUA_ROCK_TREE)/cai-prefix
LUA_LONEJSON_ROCK_STAMP := $(LUA_ROCK_TREE)/lib/luarocks/rocks-5.5/lonejson/$(CAI_LONEJSON_VERSION)-1/rock_manifest
LUA_PSLOG_ROCK_STAMP := $(LUA_ROCK_TREE)/lib/luarocks/rocks-5.5/lua-pslog/$(CAI_PSLOG_VERSION)-1/rock_manifest
RELEASE_LUA_ROCK_DIR := dist/lua-rock
RELEASE_LUA_STAGE_DIR := $(RELEASE_LUA_ROCK_DIR)/cai-$(RELEASE_VERSION)
RELEASE_LUA_SOURCE_TARBALL := dist/cai-lua-$(RELEASE_VERSION).tar.gz
RELEASE_LUA_ROCKSPEC := dist/cai-$(RELEASE_VERSION)-1.rockspec
RELEASE_LUA_PACK_DIR := dist/.lua-rock-pack
RELEASE_LUA_PACK_STAGE_DIR := $(RELEASE_LUA_PACK_DIR)/cai-$(RELEASE_VERSION)
RELEASE_LUA_PACK_SOURCE_TARBALL := $(RELEASE_LUA_PACK_DIR)/cai-lua-$(RELEASE_VERSION).tar.gz
RELEASE_LUA_PACK_ROCKSPEC := $(RELEASE_LUA_PACK_DIR)/cai-$(RELEASE_VERSION)-1.rockspec
RELEASE_LUA_SRC_ROCK := dist/cai-$(RELEASE_VERSION)-1.src.rock
RELEASE_LIVE_GATE_STAMP ?= .cache/release-gates/prerelease-live.stamp
LUA_ROCK_SOURCE_INPUTS := scripts/stage_lua_rock_sources.sh scripts/build_lua_rock.sh scripts/render_release_rockspec.sh lua/cai_lua.c cai.rockspec.in README.md LICENSE docs/model-metadata.md include/cai/blob_store.h include/cai/cai.h include/cai/mcp.h include/cai/models.h include/cai/tools/revgeo.h include/cai/tools/searxng.h include/cai/tools/todo.h
LUA_ROCK_NATIVE_INPUTS := $(shell find src include -type f \( -name '*.c' -o -name '*.h' \) | sort)

.PHONY: help deps-debug deps-release deps-cross build build-debug build-host build-release cross-build integration-build chatgpt-login test test-debug test-host test-release test-cross cross-test test-all test-e2e test-integration test-lua-smith-e2e test-install-tree asan test-asan valgrind fuzz fuzz-smoke fuzz-long coverage test-coverage example-smoke-local example-smoke-live finalize-slice clangd-check prerelease release-pipeline prerelease-live require-prerelease-live require-clean-worktree prerelease-hardening lifecycle-version-contract lua-rock lua-env lua-test release-lua-artifacts print-release-version package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-matrix release compose-check dev-up dev-down dev-reset dev-ps dev-logs searxng-pull searxng-up searxng-wait searxng-down searxng-logs searxng-test mcp-everything-up mcp-everything-wait mcp-everything-down mcp-everything-logs mcp-everything-test mcp-everything-live-test mcp-inspector-e2e format clean clean-dist

help:
	@printf '%s\n' \
		'make deps-debug  Configure the debug dependency/build root.' \
		'make deps-release Configure the pinned native release build root.' \
		'make deps-cross  Configure all available release cross dependency/build roots.' \
		'make build        Configure and build the debug preset.' \
		'make build-debug  Configure and build the debug preset.' \
		'make build-host   Build the pinned native release preset.' \
		'make build-release Configure and build the release target matrix.' \
		'make cross-build  Run the standard release cross matrix build script.' \
		'make integration-build  Configure and build the integration preset without running live tests.' \
		'make chatgpt-login  Sign CAI into ChatGPT and persist CAI-owned XDG state auth.' \
		'make test         Build and run the debug unit tests.' \
		'make test-debug   Build and run the debug unit tests.' \
		'make test-all     Run broad local confidence gates.' \
		'make test-e2e     Run deterministic compose-backed local e2e.' \
		'make test-host    Build and run the pinned native release unit tests.' \
		'make test-release Build and run the release unit tests.' \
		'make test-cross   Build cross targets; execution is target/tooling dependent.' \
		'make cross-test   Run the standard release cross matrix test script.' \
		'make test-integration  Run opt-in ChatGPT-subscription integration tests; requires CAI_ENABLE_INTEGRATION_TESTS=1.' \
		'make test-lua-smith-e2e Run the opt-in Lua Smith coding/review e2e.' \
		'make test-install-tree Verify installed SDK metadata and downstream consumers.' \
		'make asan         Build and run optional Bootlin ASan/UBSan unit tests.' \
		'make test-asan    Alias for asan.' \
		'make valgrind     Run the native Bootlin Valgrind memory-check subset.' \
		'make fuzz         Run bounded AFL++ fuzzing; CAI_FUZZ_SECONDS controls each target.' \
		'make fuzz-smoke   Replay every checked-in corpus through AFL++ instrumented harnesses.' \
		'make fuzz-long    Run extended AFL++ fuzzing; CAI_FUZZ_LONG_SECONDS controls each target.' \
		'make coverage     Build the coverage preset.' \
		'make test-coverage Run the coverage preset tests.' \
		'make example-smoke-local  Run deterministic local example smoke checks.' \
		'make example-smoke-live   Removed: use subscription-backed Smith e2e instead.' \
		'make finalize-slice Run format and debug tests before committing a slice.' \
		'make clangd-check Run host clangd validation against the native debug compile database.' \
		'make prerelease   Run the complete local release proof without cleaning first.' \
		'make prerelease-live  Run required pre-release live-provider verification; requires CAI_ENABLE_INTEGRATION_TESTS=1.' \
		'make require-prerelease-live Verify prerelease-live passed for the current commit.' \
		'make require-clean-worktree Verify tracked and untracked source files are clean.' \
		'make prerelease-hardening Run prerelease, live checks, long fuzz, and release matrix; requires CAI_ENABLE_INTEGRATION_TESTS=1.' \
		'make lifecycle-version-contract Verify exact lightweight-tag release version behavior.' \
		'make lua-rock     Build and install the LuaRock into build/luarocks.' \
		'make lua-env      Print shell exports for running local Lua examples.' \
		'make lua-test     Build the LuaRock and run the Lua binding tests.' \
		'make release-lua-artifacts Generate dist LuaRock source artifacts.' \
		'make print-release-version Print the exact packaging/release version.' \
		'make package      Build release and write dist/cai-*.tar.gz.' \
		'make package-source Build the source-only release tarball.' \
		'make package-source-smoke Verify the source tarball builds from unpacked source.' \
		'make package-checksums Generate the checksum upload manifest.' \
		'make package-verify Verify release archive structure, privacy, and metadata.' \
		'make verify-release-archives Alias for package-verify.' \
		'make verify-release-privacy Alias for package-verify privacy/relocatability gate.' \
		'make release-matrix Incrementally build, test, package, and checksum release artifacts.' \
		'make release      Validate versioning, clean, and run the final local release proof.' \
		'make dev-up       Start local compose-backed development services.' \
		'make dev-down     Stop local compose-backed development services.' \
		'make dev-reset    Stop services and remove generated local service state.' \
		'make dev-ps       Show local compose-backed service state.' \
		'make dev-logs     Follow local compose-backed service logs.' \
		'make searxng-pull Pull the configured SearXNG container image.' \
		'make searxng-up   Start local SearXNG via nerdctl compose or docker compose.' \
		'make searxng-wait Wait for the local SearXNG endpoint to answer.' \
		'make searxng-test Query local SearXNG JSON search endpoint.' \
		'make searxng-down Stop local SearXNG compose service.' \
		'make searxng-logs Follow local SearXNG logs.' \
		'make mcp-everything-up Start local MCP Everything reference server.' \
		'make mcp-everything-wait Wait for local MCP Everything to initialize.' \
		'make mcp-everything-test Run the MCP client Everything reference-server e2e matrix.' \
		'make mcp-everything-live-test Run the live model MCP client tool integration test; requires CAI_ENABLE_INTEGRATION_TESTS=1.' \
		'make mcp-inspector-e2e Run opt-in MCP Inspector container e2e; requires CAI_MCP_INSPECTOR_E2E=1.' \
		'make mcp-everything-down Stop local MCP Everything compose service.' \
		'make mcp-everything-logs Follow local MCP Everything logs.' \
		'make format       Run clang-format over repo C sources.' \
		'make clean        Remove generated build outputs.' \
		'make clean-dist   Remove dist release artifacts only.'

deps-debug:
	bash ./scripts/deps.sh debug

deps-release:
	bash ./scripts/deps.sh release

deps-cross: build-release

build: build-debug

build-debug:
	bash ./scripts/build.sh debug

build-release:
	bash ./scripts/build.sh release-matrix

build-host:
	bash ./scripts/build.sh host

cross-build:
	bash ./scripts/cross_build.sh

integration-build: $(LUA_PSLOG_ROCK_STAMP)
	bash ./scripts/build.sh integration

chatgpt-login:
	$(MAKE) -C examples run-chatgpt-login

test:
	@printf '%s\n' 'Reminder: run `make format` before committing each slice, or use `make finalize-slice`.'
	$(MAKE) test-debug

test-debug: build-debug
	bash ./scripts/test.sh debug $(CTEST_FLAGS)

test-host:
	bash ./scripts/host_test.sh

test-release: build-host
	bash ./scripts/test.sh release $(CTEST_FLAGS)

test-cross: cross-test

cross-test:
	bash ./scripts/cross_test.sh

test-all:
	$(MAKE) test-debug
	$(MAKE) test-release
	$(MAKE) valgrind
	$(MAKE) fuzz-smoke
	$(MAKE) test-e2e
	$(MAKE) package-verify

test-e2e:
	bash ./scripts/test-e2e.sh

test-integration:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run integration tests without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) lua-rock
	$(MAKE) integration-build
	bash ./scripts/test.sh integration $(CTEST_FLAGS) -R '^(cai_integration_chatgpt_.*|cai_integration_openrouter_.*|cai_lua_chatgpt_.*|cai_lua_smith_e2e|cai_lua_smith_mcp_e2e)$$'

test-lua-smith-e2e:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run Lua Smith e2e without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) lua-rock
	$(MAKE) integration-build
	$(CTEST) --preset integration --output-on-failure $(CTEST_FLAGS) -R '^cai_lua_smith_e2e$$'

test-install-tree: build-debug
	$(CTEST) --preset debug --output-on-failure $(CTEST_FLAGS) -R '^cai_install_metadata_test$$'

asan:
	bash ./scripts/build.sh asan
	$(CTEST) --preset asan $(CTEST_FLAGS)

test-asan: asan

valgrind:
	bash ./scripts/build.sh valgrind
	$(CTEST) --preset valgrind $(CTEST_FLAGS)

fuzz:
	bash ./scripts/build.sh fuzz
	./scripts/fuzz.sh smoke

fuzz-smoke:
	bash ./scripts/build.sh fuzz
	$(CTEST) --test-dir build/fuzz --output-on-failure $(CTEST_FLAGS) -L fuzz

fuzz-long:
	bash ./scripts/build.sh fuzz
	./scripts/fuzz.sh long

coverage:
	bash ./scripts/build.sh coverage

test-coverage: coverage
	$(CTEST) --preset coverage $(CTEST_FLAGS)

example-smoke-local: build-debug
	$(CTEST) --preset debug --output-on-failure $(CTEST_FLAGS) -L example-smoke

example-smoke-live:
	@printf '%s\n' 'Live example smoke is disabled: it uses API-key authentication. Use make test-integration for subscription-backed coverage.' >&2
	@exit 2

finalize-slice:
	$(MAKE) format
	$(MAKE) test-debug

clangd-check: build-debug
	$(CMAKE) -DCAI_SOURCE_DIR="$(ROOT)" -DCAI_BUILD_DIR="$(ROOT)/build/debug" -P cmake/clangd_check.cmake

release-pipeline:
	$(MAKE) format
	$(MAKE) require-clean-worktree
	$(MAKE) test-debug
	$(MAKE) integration-build
	$(MAKE) valgrind
	$(MAKE) fuzz-smoke
	$(MAKE) lua-test
	$(MAKE) test-e2e
	$(MAKE) example-smoke-local
	$(MAKE) release-matrix

prerelease: release-pipeline

prerelease-live:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run prerelease-live without CAI_ENABLE_INTEGRATION_TESTS=1' >&2; \
		exit 2; \
	fi
	$(MAKE) require-clean-worktree
	rm -f "$(RELEASE_LIVE_GATE_STAMP)"
	$(MAKE) test-integration
	$(MAKE) require-clean-worktree
	@mkdir -p "$$(dirname "$(RELEASE_LIVE_GATE_STAMP)")"
	@head="$$(git rev-parse HEAD 2>/dev/null || printf unknown)"; \
	status_sha="$$(git status --porcelain=v1 --untracked-files=all 2>/dev/null | git hash-object --stdin 2>/dev/null || printf unknown)"; \
	{ \
		printf 'status=passed\n'; \
		printf 'head=%s\n' "$$head"; \
		printf 'worktree-status-sha=%s\n' "$$status_sha"; \
		printf 'target=prerelease-live\n'; \
		printf 'timestamp=%s\n' "$$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	} >"$(RELEASE_LIVE_GATE_STAMP)"

require-prerelease-live:
	@head="$$(git rev-parse HEAD 2>/dev/null || printf unknown)"; \
	status_sha="$$(git status --porcelain=v1 --untracked-files=all 2>/dev/null | git hash-object --stdin 2>/dev/null || printf unknown)"; \
	dirty="$$(git status --porcelain=v1 --untracked-files=all 2>/dev/null || true)"; \
	if [[ ! -f "$(RELEASE_LIVE_GATE_STAMP)" ]]; then \
		printf '%s\n' 'Refusing release: run make prerelease-live first.' >&2; \
		exit 2; \
	fi; \
	if ! grep -qx 'status=passed' "$(RELEASE_LIVE_GATE_STAMP)"; then \
		printf '%s\n' 'Refusing release: prerelease-live gate stamp is not successful.' >&2; \
		exit 2; \
	fi; \
	stamp_head="$$(sed -n 's/^head=//p' "$(RELEASE_LIVE_GATE_STAMP)")"; \
	if [[ "$$stamp_head" != "$$head" ]]; then \
		printf 'Refusing release: prerelease-live passed for %s, not current HEAD %s.\n' "$$stamp_head" "$$head" >&2; \
		exit 2; \
	fi; \
	stamp_status_sha="$$(sed -n 's/^worktree-status-sha=//p' "$(RELEASE_LIVE_GATE_STAMP)")"; \
	if [[ "$$stamp_status_sha" != "$$status_sha" ]]; then \
		printf '%s\n' 'Refusing release: worktree state changed since prerelease-live.' >&2; \
		exit 2; \
	fi; \
	if [[ -n "$$dirty" ]]; then \
		printf '%s\n' 'Refusing release: worktree has uncommitted source changes.' >&2; \
		git status --short >&2; \
		exit 2; \
	fi

require-clean-worktree:
	@dirty="$$(git status --porcelain=v1 --untracked-files=all 2>/dev/null || true)"; \
	if [[ -n "$$dirty" ]]; then \
		printf '%s\n' 'Refusing release gate: worktree has uncommitted source changes.' >&2; \
		git status --short >&2; \
		exit 2; \
	fi

prerelease-hardening:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run prerelease-hardening without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) prerelease
	$(MAKE) prerelease-live
	$(MAKE) fuzz-long

lifecycle-version-contract:
	@set -euo pipefail; \
	reserved_tag='v99.99.99'; \
	cleanup() { git tag -d "$$reserved_tag" >/dev/null 2>&1 || true; }; \
	trap cleanup EXIT HUP INT TERM; \
	git tag -d "$$reserved_tag" >/dev/null 2>&1 || true; \
	exact_tag="$$(git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)"; \
	if [[ -n "$$exact_tag" ]]; then \
		tag_type="$$(git cat-file -t "refs/tags/$$exact_tag" 2>/dev/null || true)"; \
		if [[ "$$tag_type" != 'commit' ]]; then \
			printf 'release version contract: exact tag %s must be lightweight, got %s\n' "$$exact_tag" "${tag_type:-unknown}" >&2; \
			exit 1; \
		fi; \
		expected_version="$${exact_tag#v}"; \
	else \
		untagged_version="$$(./scripts/release_version.sh "$(CURDIR)")"; \
		if [[ "$$untagged_version" != '0.0.0' ]]; then \
			printf 'release version contract: untagged HEAD resolved to %s, expected 0.0.0\n' "$$untagged_version" >&2; \
			exit 1; \
		fi; \
		git -c tag.gpgSign=false tag "$$reserved_tag"; \
		if [[ "$$(git cat-file -t "$$reserved_tag")" != 'commit' ]]; then \
			printf 'release version contract: temporary tag must be lightweight\n' >&2; \
			exit 1; \
		fi; \
		expected_version="$${reserved_tag#v}"; \
	fi; \
	script_version="$$(./scripts/release_version.sh "$(CURDIR)")"; \
	make_version="$$(env -u MAKEFLAGS -u MFLAGS $(MAKE) -s print-release-version)"; \
	if [[ "$$script_version" != "$$expected_version" || "$$make_version" != "$$expected_version" ]]; then \
		printf 'release version contract: script=%s make=%s expected=%s\n' "$$script_version" "$$make_version" "$$expected_version" >&2; \
		exit 1; \
	fi

$(LUA_ROCKSPEC): cai.rockspec.in scripts/render_release_rockspec.sh | build-debug
	mkdir -p "$(LUA_ROCK_TREE)"
	lib_ext="$$(luarocks config variables.LIB_EXTENSION)"; ./scripts/render_release_rockspec.sh "$(RELEASE_VERSION)" "$(LUA_ROCKSPEC)" "git+file://$(CURDIR)" "" "$$lib_ext" ""

$(LUA_LONEJSON_ROCK_STAMP): deps-debug
	mkdir -p "$(LUA_ROCK_TREE)"
	PKG_CONFIG_PATH="$(CAI_LONEJSON_PREFIX)/lib/pkgconfig:$${PKG_CONFIG_PATH:-}" \
	CFLAGS="$${CFLAGS:+$$CFLAGS }-I$(CAI_LONEJSON_PREFIX)/include" \
	LDFLAGS="$${LDFLAGS:+$$LDFLAGS }-L$(CAI_LONEJSON_PREFIX)/lib" \
	LD_LIBRARY_PATH="$(CAI_LONEJSON_PREFIX)/lib:$${LD_LIBRARY_PATH:-}" \
	luarocks install --tree "$(LUA_ROCK_TREE)" "$(LONEJSON_LUA_ROCK_URL)"

$(LUA_PSLOG_ROCK_STAMP): deps-debug
	mkdir -p "$(LUA_ROCK_TREE)"
	PKG_CONFIG_PATH="$(CAI_PSLOG_PREFIX)/lib/pkgconfig:$${PKG_CONFIG_PATH:-}" \
	CFLAGS="$${CFLAGS:+$$CFLAGS }-fPIC -I$(CAI_PSLOG_PREFIX)/include" \
	LDFLAGS="$${LDFLAGS:+$$LDFLAGS }-L$(CAI_PSLOG_PREFIX)/lib" \
	LD_LIBRARY_PATH="$(CAI_PSLOG_PREFIX)/lib:$${LD_LIBRARY_PATH:-}" \
	luarocks install --tree "$(LUA_ROCK_TREE)" "$(PSLOG_LUA_ROCK_URL)" LIBPSLOG_DIR="$(CAI_PSLOG_PREFIX)"

$(LUA_ROCK_STAMP): $(LUA_ROCKSPEC) $(LUA_LONEJSON_ROCK_STAMP) $(LUA_PSLOG_ROCK_STAMP) lua/cai_lua.c scripts/build_lua_rock.sh $(LUA_ROCK_NATIVE_INPUTS)
	$(CMAKE) --install build/debug --prefix "$(LUA_ROCK_PREFIX)"
	flock "$(LUA_ROCK_BUILD_LOCK)" bash -lc 'set -e; export PKG_CONFIG_PATH="$(LUA_ROCK_PREFIX)/lib/pkgconfig:$(CAI_LONEJSON_PREFIX)/lib/pkgconfig:$(CAI_PSLOG_PREFIX)/lib/pkgconfig:$(CAI_C_PKT_SYSTEMS_PREFIX)/lib/pkgconfig:$${PKG_CONFIG_PATH:-}"; export PSLOG_LUA_INCLUDE_DIR="$(ROOT)/$(LUA_ROCK_TREE)/share/lua/5.5"; CFLAGS="$${CFLAGS:+$$CFLAGS }$(LUA_ROCK_EXTRA_CFLAGS)" luarocks make --tree "$(LUA_ROCK_TREE)" "$(LUA_ROCKSPEC)"; rm -rf .luarocks-build; touch "$(LUA_ROCK_STAMP)"'

lua-rock: $(LUA_ROCK_STAMP)

lua-env:
	printf '%s\n' 'eval "$$(luarocks path --tree "$(ROOT)/$(LUA_ROCK_TREE)")"'; \
	printf 'export LD_LIBRARY_PATH="%s:%s:%s:%s:$${LD_LIBRARY_PATH:-}"\n' \
		"$(ROOT)/$(LUA_ROCK_PREFIX)/lib" \
		"$(CAI_LONEJSON_PREFIX)/lib" \
		"$(CAI_C_PKT_SYSTEMS_PREFIX)/lib" \
		"$(CAI_PSLOG_PREFIX)/lib"

lua-test: lua-rock
	$(CMAKE) --preset debug-lua
	$(CMAKE) --build --preset debug-lua --target cai_lua_native_todo_store_test
	eval "$$(luarocks path --tree $(LUA_ROCK_TREE))" && \
	LUA_CPATH="$(ROOT)/build/debug-lua/lua-test/?.so;$(ROOT)/build/debug-lua/lua-test/?.dylib;$${LUA_CPATH:-}" \
	LD_LIBRARY_PATH="$(LUA_ROCK_PREFIX)/lib:$(CAI_LONEJSON_PREFIX)/lib:$(CAI_C_PKT_SYSTEMS_PREFIX)/lib:$(CAI_PSLOG_PREFIX)/lib:$${LD_LIBRARY_PATH:-}" \
	lua tests/lua/test_lua.lua
	$(CMAKE) --build build/debug --target cai_mcp_http_server
	eval "$$(luarocks path --tree $(LUA_ROCK_TREE))" && \
	LD_LIBRARY_PATH="$(LUA_ROCK_PREFIX)/lib:$(CAI_LONEJSON_PREFIX)/lib:$(CAI_C_PKT_SYSTEMS_PREFIX)/lib:$(CAI_PSLOG_PREFIX)/lib:$${LD_LIBRARY_PATH:-}" \
	/bin/sh tests/lua_mcp_client_e2e.sh build/debug/cai_mcp_http_server lua tests/lua/e2e_mcp_client.lua

$(RELEASE_LUA_SOURCE_TARBALL): $(LUA_ROCK_SOURCE_INPUTS)
	rm -rf "$(RELEASE_LUA_ROCK_DIR)" "$(RELEASE_LUA_SOURCE_TARBALL)"
	mkdir -p "$(RELEASE_LUA_ROCK_DIR)"
	./scripts/stage_lua_rock_sources.sh "$(CURDIR)" "$(RELEASE_LUA_STAGE_DIR)" "$(RELEASE_VERSION)"
	tar -C "$(RELEASE_LUA_ROCK_DIR)" --format=gnu --owner=0 --group=0 -cf "dist/cai-lua-$(RELEASE_VERSION).tar" "cai-$(RELEASE_VERSION)"
	gzip -9 -f -n "dist/cai-lua-$(RELEASE_VERSION).tar"
	rm -rf "$(RELEASE_LUA_ROCK_DIR)"

$(RELEASE_LUA_ROCKSPEC): $(RELEASE_LUA_SOURCE_TARBALL) scripts/render_release_rockspec.sh
	lib_ext="$$(luarocks config variables.LIB_EXTENSION)"; ./scripts/render_release_rockspec.sh "$(RELEASE_VERSION)" "$(RELEASE_LUA_ROCKSPEC)" "file://$(notdir $(RELEASE_LUA_SOURCE_TARBALL))" "" "$$lib_ext" "cai-$(RELEASE_VERSION)"

$(RELEASE_LUA_PACK_SOURCE_TARBALL): $(LUA_ROCK_SOURCE_INPUTS)
	rm -rf "$(RELEASE_LUA_PACK_DIR)"
	mkdir -p "$(RELEASE_LUA_PACK_DIR)"
	./scripts/stage_lua_rock_sources.sh "$(CURDIR)" "$(RELEASE_LUA_PACK_STAGE_DIR)" "$(RELEASE_VERSION)"
	tar -C "$(RELEASE_LUA_PACK_DIR)" --format=gnu --owner=0 --group=0 -cf "$(RELEASE_LUA_PACK_DIR)/cai-lua-$(RELEASE_VERSION).tar" "cai-$(RELEASE_VERSION)"
	gzip -9 -f -n "$(RELEASE_LUA_PACK_DIR)/cai-lua-$(RELEASE_VERSION).tar"

$(RELEASE_LUA_PACK_ROCKSPEC): Makefile $(RELEASE_LUA_PACK_SOURCE_TARBALL) scripts/render_release_rockspec.sh
	cd "$(RELEASE_LUA_PACK_STAGE_DIR)" && lib_ext="$$(luarocks config variables.LIB_EXTENSION)" && ./scripts/render_release_rockspec.sh "$(RELEASE_VERSION)" "../$(notdir $(RELEASE_LUA_PACK_ROCKSPEC))" "file://$(CURDIR)/$(RELEASE_LUA_PACK_SOURCE_TARBALL)" "" "$$lib_ext" "cai-$(RELEASE_VERSION)"

$(RELEASE_LUA_SRC_ROCK): $(RELEASE_LUA_PACK_ROCKSPEC) $(RELEASE_LUA_ROCKSPEC)
	rm -f "$(RELEASE_LUA_SRC_ROCK)"
	cd "$(RELEASE_LUA_PACK_DIR)" && luarocks pack "$(notdir $(RELEASE_LUA_PACK_ROCKSPEC))"
	mv "$(RELEASE_LUA_PACK_DIR)/$(notdir $(RELEASE_LUA_SRC_ROCK))" "$(RELEASE_LUA_SRC_ROCK)"
	@tmp_dir="$$(mktemp -d)"; \
	trap 'rm -rf "$$tmp_dir"' EXIT; \
	lib_ext="$$(luarocks config variables.LIB_EXTENSION)"; \
	./scripts/render_release_rockspec.sh "$(RELEASE_VERSION)" "$$tmp_dir/$(notdir $(RELEASE_LUA_PACK_ROCKSPEC))" "file://$(notdir $(RELEASE_LUA_SOURCE_TARBALL))" "" "$$lib_ext" "cai-$(RELEASE_VERSION)"; \
	zip -q -d "$(RELEASE_LUA_SRC_ROCK)" "$(notdir $(RELEASE_LUA_PACK_ROCKSPEC))"; \
	cd "$$tmp_dir" && zip -q "$(CURDIR)/$(RELEASE_LUA_SRC_ROCK)" "$(notdir $(RELEASE_LUA_PACK_ROCKSPEC))"
	rm -rf "$(RELEASE_LUA_PACK_DIR)"

release-lua-artifacts: $(RELEASE_LUA_ROCKSPEC) $(RELEASE_LUA_SRC_ROCK)

print-release-version:
	@printf '%s\n' "$(RELEASE_VERSION)"

package:
	$(MAKE) clean-dist
	$(MAKE) build-release
	bash ./scripts/package.sh release-matrix

package-source:
	bash ./scripts/build.sh package-source

package-source-smoke: package-source
	bash ./scripts/test_release_from_source.sh "$(ROOT)" "$(ROOT)/dist/cai-$(shell sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h).tar.gz"

package-checksums: package release-lua-artifacts
	$(CMAKE) -DCAI_DIST_DIR="$(ROOT)/dist" -DCAI_VERSION="$(RELEASE_VERSION)" -P cmake/package_checksums.cmake

package-verify: package-checksums
	bash ./scripts/package-verify.sh "$(ROOT)" "$$(sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h)"

verify-release-archives: package-verify

verify-release-privacy: package-verify

release-matrix:
	$(MAKE) clean-dist
	$(MAKE) build-release
	$(CTEST) --test-dir build/x86_64-linux-gnu-release --output-on-failure $(CTEST_FLAGS)
	bash ./scripts/package.sh release-matrix
	$(MAKE) package-source-smoke
	$(MAKE) release-lua-artifacts
	$(CMAKE) -DCAI_DIST_DIR="$(ROOT)/dist" -DCAI_VERSION="$(RELEASE_VERSION)" -P cmake/package_checksums.cmake
	bash ./scripts/package-verify.sh "$(ROOT)" "$$(sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h)"

release:
	$(MAKE) lifecycle-version-contract
	$(MAKE) clean
	$(MAKE) release-pipeline

compose-check:
	@$(COMPOSE) version >/dev/null

dev-up:
	bash ./scripts/dev-up.sh

dev-down:
	bash ./scripts/dev-down.sh

dev-reset:
	bash ./scripts/dev-reset.sh

dev-ps:
	bash ./scripts/dev-ps.sh

dev-logs:
	bash ./scripts/dev-logs.sh

searxng-pull: compose-check
	$(COMPOSE) pull searxng

searxng-up: compose-check
	$(COMPOSE) pull searxng
	$(COMPOSE) up -d searxng

searxng-wait:
	@url="$${CAI_SEARXNG_BASE_URL:-$(CAI_SEARXNG_BASE_URL)}/"; \
	for attempt in {1..30}; do \
		if curl -fsS "$$url" >/dev/null; then \
			printf 'SearXNG is ready at %s\n' "$$url"; \
			exit 0; \
		fi; \
		sleep 1; \
	done; \
	printf 'Timed out waiting for SearXNG at %s\n' "$$url" >&2; \
	exit 1

searxng-down: compose-check
	$(COMPOSE) stop searxng
	$(COMPOSE) rm -f searxng

searxng-logs: compose-check
	$(COMPOSE) logs -f searxng

searxng-test:
	@url="$${CAI_SEARXNG_BASE_URL:-$(CAI_SEARXNG_BASE_URL)}/search"; \
	engine="$${CAI_SEARXNG_TEST_ENGINE:-$(CAI_SEARXNG_TEST_ENGINE)}"; \
	query="$${CAI_SEARXNG_TEST_QUERY:-$(CAI_SEARXNG_TEST_QUERY)}"; \
	printf 'GET %s?q=%s&format=json&engines=%s\n' "$$url" "$$query" "$$engine"; \
	curl -fsS --get "$$url" \
		--data-urlencode "q=$$query" \
		--data-urlencode "format=json" \
		--data-urlencode "engines=$$engine" \
		--data-urlencode "safesearch=0" \
		--data-urlencode "language=en" | head -c "$${CAI_SEARXNG_TEST_BYTES:-2000}"; \
	printf '\n'

mcp-everything-up: compose-check
	$(COMPOSE) up -d --build mcp-everything

mcp-everything-wait:
	@url="$${CAI_MCP_EVERYTHING_BASE_URL:-$(CAI_MCP_EVERYTHING_BASE_URL)}"; \
	tmpdir="$$(mktemp -d)"; \
	trap 'rm -rf "$$tmpdir"' EXIT; \
	init='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cai-compose-wait","version":"0.0.0"}}}'; \
	attempt=1; \
	while [ "$$attempt" -le 30 ]; do \
		if curl -fsS -D "$$tmpdir/headers" -o "$$tmpdir/body" \
			-H 'content-type: application/json' \
			-H 'accept: application/json, text/event-stream' \
			-d "$$init" "$$url" >/dev/null 2>&1 && \
			grep -q '"serverInfo"' "$$tmpdir/body"; then \
			printf 'MCP Everything is ready at %s\n' "$$url"; \
			exit 0; \
		fi; \
		attempt=$$((attempt + 1)); \
		sleep 1; \
	done; \
	printf 'Timed out waiting for MCP Everything at %s\n' "$$url" >&2; \
	exit 1

mcp-everything-down: compose-check
	$(COMPOSE) stop mcp-everything
	$(COMPOSE) rm -f mcp-everything

mcp-everything-logs: compose-check
	$(COMPOSE) logs -f mcp-everything

mcp-everything-test: build-debug
	$(CMAKE) --build build/debug --target cai_mcp_everything_e2e
	@url="$${CAI_MCP_EVERYTHING_BASE_URL:-$(CAI_MCP_EVERYTHING_BASE_URL)}"; \
	build/debug/cai_mcp_everything_e2e "$$url"

mcp-everything-live-test:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run live MCP Everything test without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) integration-build
	$(CTEST) --preset integration --output-on-failure $(CTEST_FLAGS) -R '^cai_integration_chatgpt_mcp_client_tool$$'

mcp-inspector-e2e:
	@if [[ "$${CAI_MCP_INSPECTOR_E2E:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run MCP Inspector e2e without CAI_MCP_INSPECTOR_E2E=1'; \
		exit 2; \
	fi
	$(MAKE) build-debug
	CAI_MCP_INSPECTOR_IMAGE="$(CAI_MCP_INSPECTOR_IMAGE)" \
	$(CTEST) --preset debug --output-on-failure $(CTEST_FLAGS) -R '^cai_mcp_inspector_e2e$$'

format:
	bash ./scripts/build.sh configure-debug
	$(CMAKE) --build build/debug --target clang-format

clean:
	bash ./scripts/clean.sh

clean-dist:
	bash ./scripts/clean.sh dist
