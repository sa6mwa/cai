#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
presets_file=${CAI_HOST_RELEASE_PRESETS_FILE:-"$repo_root/CMakePresets.json"}
uname_s=${CAI_HOST_RELEASE_UNAME_S:-"$(uname -s)"}
uname_m=${CAI_HOST_RELEASE_UNAME_M:-"$(uname -m)"}
cc=${CC:-cc}
cc_machine=${CAI_HOST_RELEASE_CC_MACHINE:-""}

if [[ -z "$cc_machine" && "$uname_s" == "Linux" ]]; then
  cc_machine=$("$cc" -dumpmachine 2>/dev/null || true)
fi

case "$uname_m" in
  x86_64 | amd64)
    arch=x86_64
    ;;
  aarch64 | arm64)
    arch=aarch64
    ;;
  armv7* | armv6* | armhf)
    arch=armhf
    ;;
  *)
    printf 'unsupported host architecture for release preset: %s\n' "$uname_m" >&2
    exit 1
    ;;
esac

case "$uname_s" in
  Linux)
    libc=gnu
    if [[ "$cc_machine" == *musl* ]]; then
      libc=musl
    fi
    target="${arch}-linux-${libc}"
    case "$target" in
      x86_64-linux-gnu)
        ;;
      *)
        target="${target}-host"
        ;;
    esac
    ;;
  Darwin)
    if [[ "$arch" == "aarch64" ]]; then
      target=arm64-apple-darwin-host
    else
      printf 'unsupported host release preset for Darwin architecture without pinned dependencies: %s\n' "$uname_m" >&2
      exit 1
    fi
    ;;
  *)
    printf 'unsupported host OS for release preset: %s\n' "$uname_s" >&2
    exit 1
    ;;
esac

preset="${target}-release"
if ! grep -Eq "\"name\"[[:space:]]*:[[:space:]]*\"${preset}\"" "$presets_file"; then
  printf 'host release preset is not available: %s\n' "$preset" >&2
  exit 1
fi

printf '%s\n' "$preset"
