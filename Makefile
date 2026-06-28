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
CAI_FUZZ_RUNS ?= 10000
RELEASE_VERSION ?= $(shell ./scripts/detect_release_version.sh "$(CURDIR)")
CAI_CPKT_TARGET ?= x86_64-linux-gnu
CAI_C_PKT_SYSTEMS_VERSION ?= 0.6.0
CAI_LONEJSON_VERSION ?= 0.35.1
CAI_PSLOG_VERSION ?= 0.4.1
LONEJSON_LUA_ROCK_URL ?= https://github.com/sa6mwa/lonejson/releases/download/v$(CAI_LONEJSON_VERSION)/lonejson-$(CAI_LONEJSON_VERSION)-1.src.rock
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
RELEASE_LUA_ROCK_DIR := dist/lua-rock
RELEASE_LUA_STAGE_DIR := $(RELEASE_LUA_ROCK_DIR)/cai-$(RELEASE_VERSION)
RELEASE_LUA_SOURCE_TARBALL := dist/cai-lua-$(RELEASE_VERSION).tar.gz
RELEASE_LUA_ROCKSPEC := dist/cai-$(RELEASE_VERSION)-1.rockspec
RELEASE_LUA_PACK_DIR := dist/.lua-rock-pack
RELEASE_LUA_PACK_STAGE_DIR := $(RELEASE_LUA_PACK_DIR)/cai-$(RELEASE_VERSION)
RELEASE_LUA_PACK_SOURCE_TARBALL := $(RELEASE_LUA_PACK_DIR)/cai-lua-$(RELEASE_VERSION).tar.gz
RELEASE_LUA_PACK_ROCKSPEC := $(RELEASE_LUA_PACK_DIR)/cai-$(RELEASE_VERSION)-1.rockspec
RELEASE_LUA_SRC_ROCK := dist/cai-$(RELEASE_VERSION)-1.src.rock
LUA_ROCK_SOURCE_INPUTS := scripts/stage_lua_rock_sources.sh lua/cai_lua.c cai.rockspec.in README.md LICENSE include/cai/cai.h include/cai/mcp.h include/cai/models.h include/cai/tools/revgeo.h include/cai/tools/searxng.h include/cai/tools/todo.h
LUA_ROCK_NATIVE_INPUTS := $(shell find src include -type f \( -name '*.c' -o -name '*.h' \) | sort)

.PHONY: help deps-debug deps-release deps-cross build build-debug build-host build-release cross-build integration-build test test-debug test-host test-release test-cross cross-test test-all test-e2e test-integration asan test-asan tsan test-tsan msan test-msan fuzz fuzz-smoke fuzz-full fuzz-long coverage test-coverage example-smoke-local example-smoke-live finalize-slice prerelease prerelease-live prerelease-hardening lua-rock lua-env lua-test release-lua-artifacts print-release-version package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-matrix release compose-check dev-up dev-down dev-reset dev-ps dev-logs searxng-pull searxng-up searxng-wait searxng-down searxng-logs searxng-test mcp-everything-up mcp-everything-wait mcp-everything-down mcp-everything-logs mcp-everything-test mcp-everything-live-test format clean clean-dist

help:
	@printf '%s\n' \
		'make deps-debug  Configure the debug dependency/build root.' \
		'make deps-release Configure the host release dependency/build root.' \
		'make deps-cross  Configure all available release cross dependency/build roots.' \
		'make build        Configure and build the debug preset.' \
		'make build-host   Alias for build-release host artifact build.' \
		'make build-release Configure and build the release preset.' \
		'make cross-build  Alias for build-release cross matrix build.' \
		'make integration-build  Configure and build the integration preset without running live tests.' \
		'make test         Build and run the debug unit tests.' \
		'make test-all     Run broad local confidence gates.' \
		'make test-e2e     Run deterministic compose-backed local e2e.' \
		'make test-release Build and run the release unit tests.' \
		'make test-cross   Build cross targets; execution is target/tooling dependent.' \
		'make test-integration  Run opt-in OpenAI API integration tests.' \
		'make asan         Build and run the ASan/UBSan unit tests.' \
		'make tsan         Build and run the TSan local test suite.' \
		'make msan         Build and run the MSan smoke subset.' \
		'make fuzz         Build all libFuzzer harnesses.' \
		'make fuzz-smoke   Run one-iteration smoke checks for every fuzzer.' \
		'make fuzz-full    Run every fuzzer with the checked-in corpus and CAI_FUZZ_RUNS iterations.' \
		'make fuzz-long    Alias for fuzz-full.' \
		'make coverage     Build the coverage preset.' \
		'make test-coverage Run the coverage preset tests.' \
		'make example-smoke-local  Run deterministic local example smoke checks.' \
		'make example-smoke-live   Run curated live non-interactive example smoke checks.' \
		'make finalize-slice Run format and debug tests before committing a slice.' \
		'make prerelease   Run the standard local prerelease verification tier.' \
		'make prerelease-live  Run live-provider verification; requires CAI_ENABLE_INTEGRATION_TESTS=1.' \
		'make prerelease-hardening Run the hardening tier: prerelease, live checks, long fuzz, and release matrix.' \
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
		'make release      Clean first, then run prerelease, live prerelease, and release matrix gates.' \
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
		'make mcp-everything-up Start local MCP Everything reference server.' \
		'make mcp-everything-wait Wait for local MCP Everything to initialize.' \
		'make mcp-everything-test Run the MCP client Everything reference-server e2e matrix.' \
		'make mcp-everything-live-test Run the live model MCP client tool integration test.' \
		'make mcp-everything-down Stop local MCP Everything compose service.' \
		'make format       Run clang-format over repo C sources.' \
		'make clean        Remove generated build outputs.' \
		'make clean-dist   Remove dist release artifacts only.'

deps-debug:
	$(CMAKE) --preset debug

deps-release:
	$(CMAKE) --preset x86_64-linux-gnu-release

deps-cross: build-release

build: build-debug

build-debug:
	$(CMAKE) --preset debug
	$(CMAKE) --build --preset debug

build-release:
	bash ./scripts/build_release_matrix.sh

build-host: build-release

cross-build: build-release

integration-build:
	$(CMAKE) --preset integration
	$(CMAKE) --build --preset integration

test:
	@printf '%s\n' 'Reminder: run `make format` before committing each slice, or use `make finalize-slice`.'
	$(MAKE) test-debug

test-debug: build-debug
	$(CTEST) --preset debug $(CTEST_FLAGS)

test-host: test-release

test-release: build-release
	$(CTEST) --test-dir build/x86_64-linux-gnu-release --output-on-failure $(CTEST_FLAGS)

test-cross: cross-test

cross-test: build-release

test-all:
	$(MAKE) test-debug
	$(MAKE) asan
	$(MAKE) test-e2e
	$(MAKE) package-verify

test-e2e:
	bash ./scripts/test-e2e.sh

test-integration:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run integration tests without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) integration-build
	$(CTEST) --preset integration $(CTEST_FLAGS)

asan:
	$(CMAKE) --preset asan
	$(CMAKE) --build --preset asan
	$(CTEST) --preset asan $(CTEST_FLAGS)

test-asan: asan

tsan:
	$(CMAKE) --preset tsan
	$(CMAKE) --build --preset tsan
	$(CTEST) --preset tsan $(CTEST_FLAGS)

test-tsan: tsan

msan:
	$(CMAKE) --preset msan
	$(CMAKE) --build --preset msan
	$(CTEST) --preset msan $(CTEST_FLAGS)

test-msan: msan

fuzz:
	$(CMAKE) --preset fuzz
	$(CMAKE) --build --preset fuzz

fuzz-smoke: fuzz
	$(CTEST) --test-dir build/fuzz --output-on-failure $(CTEST_FLAGS) -L fuzz

fuzz-full: fuzz
	build/fuzz/cai_tool_fuzz tests/fuzz-corpus/tool -runs=$(CAI_FUZZ_RUNS)
	build/fuzz/cai_stream_fuzz tests/fuzz-corpus/stream -runs=$(CAI_FUZZ_RUNS)
	build/fuzz/cai_response_fuzz tests/fuzz-corpus/response -runs=$(CAI_FUZZ_RUNS)
	build/fuzz/cai_mcp_fuzz tests/fuzz-corpus/mcp -runs=$(CAI_FUZZ_RUNS)
	build/fuzz/cai_session_fuzz tests/fuzz-corpus/session -runs=$(CAI_FUZZ_RUNS)
	build/fuzz/cai_todo_fuzz tests/fuzz-corpus/todo -runs=$(CAI_FUZZ_RUNS)

fuzz-long: fuzz-full

coverage:
	$(CMAKE) --preset coverage
	$(CMAKE) --build --preset coverage

test-coverage: coverage
	$(CTEST) --preset coverage $(CTEST_FLAGS)

example-smoke-local: build-debug
	$(CTEST) --preset debug --output-on-failure $(CTEST_FLAGS) -L example-smoke

example-smoke-live:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run live example smoke without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) integration-build
	$(CTEST) --preset integration --output-on-failure $(CTEST_FLAGS) -R '^cai_examples_live_smoke$$'

finalize-slice:
	$(MAKE) format
	$(MAKE) test-debug

prerelease:
	$(MAKE) format
	$(MAKE) test-debug
	$(MAKE) integration-build
	$(MAKE) tsan
	$(MAKE) msan
	$(MAKE) fuzz-smoke
	$(MAKE) lua-test
	$(MAKE) test-e2e
	$(MAKE) example-smoke-local

prerelease-live:
	$(MAKE) test-integration
	$(MAKE) example-smoke-live

prerelease-hardening:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run prerelease-hardening without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(MAKE) prerelease
	$(MAKE) prerelease-live
	$(MAKE) fuzz-full
	$(MAKE) release-matrix

$(LUA_ROCKSPEC): cai.rockspec.in scripts/render_release_rockspec.sh | build-debug
	mkdir -p "$(LUA_ROCK_TREE)"
	lib_ext="$$(luarocks config variables.LIB_EXTENSION)"; ./scripts/render_release_rockspec.sh "$(RELEASE_VERSION)" "$(LUA_ROCKSPEC)" "git+file://$(CURDIR)" "" "$$lib_ext" ""

$(LUA_LONEJSON_ROCK_STAMP):
	mkdir -p "$(LUA_ROCK_TREE)"
	PKG_CONFIG_PATH="$(CAI_LONEJSON_PREFIX)/lib/pkgconfig:$${PKG_CONFIG_PATH:-}" \
	CFLAGS="$${CFLAGS:+$$CFLAGS }-I$(CAI_LONEJSON_PREFIX)/include" \
	LDFLAGS="$${LDFLAGS:+$$LDFLAGS }-L$(CAI_LONEJSON_PREFIX)/lib" \
	LD_LIBRARY_PATH="$(CAI_LONEJSON_PREFIX)/lib:$${LD_LIBRARY_PATH:-}" \
	luarocks install --tree "$(LUA_ROCK_TREE)" "$(LONEJSON_LUA_ROCK_URL)"

$(LUA_ROCK_STAMP): $(LUA_ROCKSPEC) $(LUA_LONEJSON_ROCK_STAMP) lua/cai_lua.c scripts/build_lua_rock.sh $(LUA_ROCK_NATIVE_INPUTS)
	$(CMAKE) --install build/debug --prefix "$(LUA_ROCK_PREFIX)"
	flock "$(LUA_ROCK_BUILD_LOCK)" bash -lc 'set -e; export PKG_CONFIG_PATH="$(LUA_ROCK_PREFIX)/lib/pkgconfig:$(CAI_LONEJSON_PREFIX)/lib/pkgconfig:$(CAI_PSLOG_PREFIX)/lib/pkgconfig:$(CAI_C_PKT_SYSTEMS_PREFIX)/lib/pkgconfig:$${PKG_CONFIG_PATH:-}"; CFLAGS="$${CFLAGS:+$$CFLAGS }$(LUA_ROCK_EXTRA_CFLAGS)" luarocks make --tree "$(LUA_ROCK_TREE)" "$(LUA_ROCKSPEC)"; rm -rf .luarocks-build; touch "$(LUA_ROCK_STAMP)"'

lua-rock: $(LUA_ROCK_STAMP)

lua-env:
	@asan_lib="$$(cc -print-file-name=libasan.so 2>/dev/null || true)"; \
	if [[ ! -f "$$asan_lib" ]]; then asan_lib=""; fi; \
	printf '%s\n' 'eval "$$(luarocks path --tree "$(ROOT)/$(LUA_ROCK_TREE)")"'; \
	printf 'export LD_LIBRARY_PATH="%s:%s:%s:%s:$${LD_LIBRARY_PATH:-}"\n' \
		"$(ROOT)/$(LUA_ROCK_PREFIX)/lib" \
		"$(CAI_LONEJSON_PREFIX)/lib" \
		"$(CAI_C_PKT_SYSTEMS_PREFIX)/lib" \
		"$(CAI_PSLOG_PREFIX)/lib"; \
	if [[ -n "$$asan_lib" ]]; then \
		printf 'export LD_PRELOAD="%s$${LD_PRELOAD:+:$$LD_PRELOAD}"\n' "$$asan_lib"; \
	fi

lua-test: lua-rock
	asan_lib="$$(cc -print-file-name=libasan.so 2>/dev/null || true)"; \
	if [[ ! -f "$$asan_lib" ]]; then asan_lib=""; fi; \
	eval "$$(luarocks path --tree $(LUA_ROCK_TREE))" && \
	LD_LIBRARY_PATH="$(LUA_ROCK_PREFIX)/lib:$(CAI_LONEJSON_PREFIX)/lib:$(CAI_C_PKT_SYSTEMS_PREFIX)/lib:$${LD_LIBRARY_PATH:-}" \
	LD_PRELOAD="$${asan_lib}$${LD_PRELOAD:+:$$LD_PRELOAD}" \
	lua tests/lua/test_lua.lua

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
	cd "$$tmp_dir" && zip -q -u "$(CURDIR)/$(RELEASE_LUA_SRC_ROCK)" "$(notdir $(RELEASE_LUA_PACK_ROCKSPEC))"
	rm -rf "$(RELEASE_LUA_PACK_DIR)"

release-lua-artifacts: $(RELEASE_LUA_ROCKSPEC) $(RELEASE_LUA_SRC_ROCK)

print-release-version:
	@printf '%s\n' "$(RELEASE_VERSION)"

package: build-release
	bash ./scripts/package_release_matrix.sh

package-source:
	$(CMAKE) --preset x86_64-linux-gnu-release
	$(CMAKE) --build --preset x86_64-linux-gnu-release --target cai_package_source

package-source-smoke: package-source
	bash ./scripts/test_release_source.sh "$(ROOT)" "$(ROOT)/dist/cai-$(shell sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h).tar.gz"

package-checksums: package release-lua-artifacts
	$(CMAKE) -DCAI_DIST_DIR="$(ROOT)/dist" -DCAI_VERSION="$(RELEASE_VERSION)" -P cmake/package_checksums.cmake

package-verify: package-checksums
	bash ./scripts/verify_release_artifacts.sh "$(ROOT)" "$$(sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h)"

verify-release-archives: package-verify

verify-release-privacy: package-verify

release-matrix:
	$(MAKE) build-release
	$(CTEST) --test-dir build/x86_64-linux-gnu-release --output-on-failure $(CTEST_FLAGS)
	bash ./scripts/package_release_matrix.sh
	$(MAKE) release-lua-artifacts
	$(CMAKE) -DCAI_DIST_DIR="$(ROOT)/dist" -DCAI_VERSION="$(RELEASE_VERSION)" -P cmake/package_checksums.cmake
	bash ./scripts/verify_release_artifacts.sh "$(ROOT)" "$$(sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h)"

release:
	$(MAKE) clean
	$(MAKE) prerelease
	CAI_ENABLE_INTEGRATION_TESTS=1 $(MAKE) prerelease-live
	$(MAKE) release-matrix

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
	$(MAKE) integration-build
	$(CTEST) --preset integration --output-on-failure $(CTEST_FLAGS) -R '^cai_integration_mcp_client_tool$$'

format:
	$(CMAKE) --preset debug
	$(CMAKE) --build build/debug --target clang-format

clean:
	$(CMAKE) -E rm -rf build dist .cache .luarocks-build

clean-dist:
	$(CMAKE) -E rm -rf dist
