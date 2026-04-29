SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules

ROOT := $(CURDIR)
CMAKE := cmake
CTEST := ctest

.PHONY: help build build-debug test test-debug test-live asan test-asan format clean

help:
	@printf '%s\n' \
		'make build        Configure and build the debug preset.' \
		'make test         Build and run the debug unit tests.' \
		'make test-live    Run opt-in live OpenAI tests when CAI_ENABLE_LIVE_TESTS=1.' \
		'make asan         Build and run the ASan/UBSan unit tests.' \
		'make format       Run clang-format over repo C sources.' \
		'make clean        Remove generated build outputs.'

build: build-debug

build-debug:
	$(CMAKE) --preset debug
	$(CMAKE) --build --preset debug

test: test-debug

test-debug: build-debug
	$(CTEST) --preset debug

test-live:
	@if [[ "$${CAI_ENABLE_LIVE_TESTS:-}" != "1" ]]; then \
		printf '%s\n' 'Refusing to run live OpenAI tests without CAI_ENABLE_LIVE_TESTS=1'; \
		exit 2; \
	fi
	$(CMAKE) --preset live
	$(CMAKE) --build --preset live
	$(CTEST) --preset live

asan:
	$(CMAKE) --preset asan
	$(CMAKE) --build --preset asan
	$(CTEST) --preset asan

test-asan: asan

format:
	$(CMAKE) --preset debug
	$(CMAKE) --build build/debug --target clang-format

clean:
	$(CMAKE) -E rm -rf build
