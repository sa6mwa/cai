SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules

ROOT := $(CURDIR)
CMAKE := cmake
CTEST := ctest

.PHONY: help build build-debug build-release test test-debug test-release test-integration asan test-asan package package-source package-source-smoke package-checksums release format clean

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

format:
	$(CMAKE) --preset debug
	$(CMAKE) --build build/debug --target clang-format

clean:
	$(CMAKE) -E rm -rf build
