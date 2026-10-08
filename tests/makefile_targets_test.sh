#!/usr/bin/env bash
set -euo pipefail
repo=$1
for target in build build-cli test test-cli test-all test-e2e test-offline test-install-tree format format-check prerelease release release-matrix package package-source package-source-smoke package-verify clean clean-dist valgrind fuzz-smoke lua-test dev-up dev-down dev-reset dev-ps dev-logs; do
  grep -Eq "^$target:" "$repo/Makefile" || { printf 'Missing Make target: %s\n' "$target" >&2; exit 1; }
done
# Formatting and artifact verification must not implicitly start producers.
! sed -n '/^format:/,/^format-check:/p' "$repo/Makefile" | grep -E 'build.sh|--build|--preset'
! sed -n '/^package-verify:/,/^verify-release-archives:/p' "$repo/Makefile" | grep -E 'build.sh|clean|package-checksums'
! grep -E 'tag .*v99|tag -d|git tag' "$repo/Makefile"
! grep -E 'python|\.py' "$repo/scripts/"*.sh | grep -v 'podman exec.*python'
! grep -E 'COMMAND python|_test\.py' "$repo/cmake/Cai"*Tests.cmake
! grep -Eq 'hostNetwork: *true|privileged: *true|image:.*:latest' "$repo/devenv.yaml.in"
[[ ! -e "$repo/scripts/devenv.py" ]]
