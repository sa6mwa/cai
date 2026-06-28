#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  printf 'usage: %s <repo-root>\n' "$0" >&2
  exit 2
fi

repo_root=$1
resolver="$repo_root/scripts/host_release_preset.sh"

expect_preset() {
  expected=$1
  shift
  actual=$(env "$@" "$resolver")
  if [ "$actual" != "$expected" ]; then
    printf 'expected host preset %s, got %s\n' "$expected" "$actual" >&2
    exit 1
  fi
}

expect_failure() {
  shift
  if output=$(env "$@" "$resolver" 2>&1); then
    printf 'expected host preset resolver to fail, got success: %s\n' "$output" >&2
    exit 1
  fi
}

expect_preset x86_64-linux-gnu-release \
  CAI_HOST_RELEASE_UNAME_S=Linux \
  CAI_HOST_RELEASE_UNAME_M=x86_64 \
  CAI_HOST_RELEASE_CC_MACHINE=x86_64-linux-gnu

expect_preset x86_64-linux-musl-release \
  CAI_HOST_RELEASE_UNAME_S=Linux \
  CAI_HOST_RELEASE_UNAME_M=x86_64 \
  CAI_HOST_RELEASE_CC_MACHINE=x86_64-linux-musl

expect_preset aarch64-linux-gnu-release \
  CAI_HOST_RELEASE_UNAME_S=Linux \
  CAI_HOST_RELEASE_UNAME_M=aarch64 \
  CAI_HOST_RELEASE_CC_MACHINE=aarch64-linux-gnu

expect_preset armhf-linux-gnu-release \
  CAI_HOST_RELEASE_UNAME_S=Linux \
  CAI_HOST_RELEASE_UNAME_M=armv7l \
  CAI_HOST_RELEASE_CC_MACHINE=arm-linux-gnueabihf

expect_preset arm64-apple-darwin-release \
  CAI_HOST_RELEASE_UNAME_S=Darwin \
  CAI_HOST_RELEASE_UNAME_M=arm64 \
  CAI_HOST_RELEASE_CC_MACHINE=

expect_failure unsupported-os \
  CAI_HOST_RELEASE_UNAME_S=FreeBSD \
  CAI_HOST_RELEASE_UNAME_M=x86_64 \
  CAI_HOST_RELEASE_CC_MACHINE=x86_64-unknown-freebsd

expect_failure unsupported-arch \
  CAI_HOST_RELEASE_UNAME_S=Linux \
  CAI_HOST_RELEASE_UNAME_M=riscv64 \
  CAI_HOST_RELEASE_CC_MACHINE=riscv64-linux-gnu
