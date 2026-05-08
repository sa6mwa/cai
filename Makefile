SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules

ROOT := $(CURDIR)
CMAKE := cmake
CTEST := ctest
COMPOSE_FILE := docker-compose.yaml
COMPOSE := $(shell if command -v nerdctl >/dev/null 2>&1; then printf 'nerdctl compose'; elif command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then printf 'docker compose'; else printf ''; fi)
CAI_SEARXNG_BASE_URL ?= http://127.0.0.1:8888
CAI_SEARXNG_TEST_QUERY ?= openai responses api

.PHONY: help build build-debug build-release test test-debug test-release test-integration asan test-asan package package-source package-source-smoke package-checksums release compose-check searxng-pull searxng-up searxng-wait searxng-down searxng-logs searxng-test format clean

help:
	@printf '%s\n' \
		'make build        Configure and build the debug preset.' \
		'make build-release Configure and build the release preset.' \
		'make test         Build and run the debug unit tests.' \
		'make test-release Build and run the release unit tests.' \
		'make test-integration  Run opt-in OpenAI API integration tests.' \
		'make asan         Build and run the ASan/UBSan unit tests.' \
		'make package      Build release and write dist/cai-*.tar.gz.' \
		'make package-source Build the source-only release tarball.' \
		'make package-source-smoke Verify the source tarball builds from unpacked source.' \
		'make release      Build, test, package, and checksum release artifacts.' \
		'make searxng-pull Pull the configured SearXNG container image.' \
		'make searxng-up   Start local SearXNG via nerdctl compose or docker compose.' \
		'make searxng-wait Wait for the local SearXNG endpoint to answer.' \
		'make searxng-test Query local SearXNG JSON search endpoint.' \
		'make searxng-down Stop local SearXNG compose service.' \
		'make format       Run clang-format over repo C sources.' \
		'make clean        Remove generated build outputs.'

build: build-debug

build-debug:
	$(CMAKE) --preset debug
	$(CMAKE) --build --preset debug

build-release:
	bash ./scripts/build_release_matrix.sh

test: test-debug

test-debug: build-debug
	$(CTEST) --preset debug

test-release: build-release
	$(CTEST) --preset release

test-integration:
	@if [[ "$${CAI_ENABLE_INTEGRATION_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run integration tests without CAI_ENABLE_INTEGRATION_TESTS=1'; \
		exit 2; \
	fi
	$(CMAKE) --preset integration
	$(CMAKE) --build --preset integration
	$(CTEST) --preset integration

asan:
	$(CMAKE) --preset asan
	$(CMAKE) --build --preset asan
	$(CTEST) --preset asan

test-asan: asan

package: build-release
	bash ./scripts/package_release_matrix.sh

package-source:
	$(CMAKE) --preset x86_64-linux-gnu-release
	$(CMAKE) --build --preset x86_64-linux-gnu-release --target cai_package_source

package-source-smoke: package-source
	bash ./scripts/test_release_source.sh "$(ROOT)" "$(ROOT)/dist/cai-$(shell sed -n 's/^#define CAI_VERSION_STRING "\(.*\)"/\1/p' build/x86_64-linux-gnu-release/generated/include/cai/version.h).tar.gz"

package-checksums: package
	$(CMAKE) --build --preset x86_64-linux-gnu-release --target cai_package_checksums

release: test-release package-checksums

compose-check:
	@if [[ -z "$(COMPOSE)" ]]; then \
		printf '%s\n' 'Neither nerdctl compose nor docker compose was found in PATH.' >&2; \
		exit 2; \
	fi

searxng-pull: compose-check
	$(COMPOSE) -f "$(COMPOSE_FILE)" pull searxng

searxng-up: compose-check
	$(COMPOSE) -f "$(COMPOSE_FILE)" pull searxng
	$(COMPOSE) -f "$(COMPOSE_FILE)" up -d searxng

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
	$(COMPOSE) -f "$(COMPOSE_FILE)" down

searxng-logs: compose-check
	$(COMPOSE) -f "$(COMPOSE_FILE)" logs -f searxng

searxng-test:
	@url="$${CAI_SEARXNG_BASE_URL:-$(CAI_SEARXNG_BASE_URL)}/search"; \
	query="$${CAI_SEARXNG_TEST_QUERY:-$(CAI_SEARXNG_TEST_QUERY)}"; \
	printf 'GET %s?q=%s&format=json\n' "$$url" "$$query"; \
	curl -fsS --get "$$url" \
		--data-urlencode "q=$$query" \
		--data-urlencode "format=json" \
		--data-urlencode "safesearch=0" \
		--data-urlencode "language=en" | head -c "$${CAI_SEARXNG_TEST_BYTES:-2000}"; \
	printf '\n'

format:
	$(CMAKE) --preset debug
	$(CMAKE) --build build/debug --target clang-format

clean:
	$(CMAKE) -E rm -rf build
