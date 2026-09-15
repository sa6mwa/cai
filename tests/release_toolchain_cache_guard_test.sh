#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 || ! -d "$2" ]]; then
  printf 'usage: %s <repo-root> <existing-build-dir>\n' "$0" >&2
  exit 2
fi

repo_root=$1
build_dir=$2
test_root=$(mktemp -d "$build_dir/toolchain-cache-guard.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/pkg"
cache_root=/var/cache
cpkt_cache_dir_name=c.pkt.systems
cpkt_toolchain_dir_name=toolchains
printf 'cache=%s/%s/%s/roots/bootlin\n' \
  "$cache_root" "$cpkt_cache_dir_name" "$cpkt_toolchain_dir_name" \
  >"$test_root/pkg/libcai.so"

if CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_no_private_bytes '$test_root/pkg'" \
  >/dev/null 2>&1; then
  printf 'expected Bootlin toolchain cache path to fail artifact verification\n' >&2
  exit 1
fi

relocated_cache=$test_root/bootlin-cache
printf 'cache=%s/roots/bootlin\n' "$relocated_cache" >"$test_root/pkg/libcai.so"
if CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 \
  CPKT_TOOLCHAIN_CACHE="$relocated_cache" bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_no_private_bytes '$test_root/pkg'" \
  >/dev/null 2>&1; then
  printf 'expected configured Bootlin toolchain cache path to fail artifact verification\n' >&2
  exit 1
fi

listing=$test_root/archive-members.txt
printf '%s\n' "cai-1.2.3${cache_root}/${cpkt_cache_dir_name}/${cpkt_toolchain_dir_name}/roots/leaked-file" \
  >"$listing"
if CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_listing_has_no_host_paths '$listing'" \
  >/dev/null 2>&1; then
  printf 'expected Bootlin toolchain cache path in archive member list to fail verification\n' >&2
  exit 1
fi

fake_readelf=$test_root/readelf
cat >"$fake_readelf" <<'EOF_READELF'
#!/usr/bin/env bash
set -euo pipefail
printf ' 0x000000000000000f (RPATH) Library rpath: [$ORIGIN:%s/lib]\n' \
  "${CAI_FAKE_RPATH:?}"
EOF_READELF
chmod +x "$fake_readelf"
mkdir -p "$test_root/elf/lib"
: >"$test_root/elf/lib/libcai.so.0.2.0"
if CAI_REPO_ROOT="$repo_root" CAI_VERSION=1.2.3 \
  CAI_READELF="$fake_readelf" CAI_FAKE_RPATH="$relocated_cache" \
  CPKT_TOOLCHAIN_CACHE="$relocated_cache" bash -c \
  "source '$repo_root/scripts/verify_release_artifacts.sh' --self-test; verify_linux_runpath '$test_root/elf' x86_64-linux-gnu" \
  >/dev/null 2>&1; then
  printf 'expected Bootlin cache path in ELF loader metadata to fail artifact verification\n' >&2
  exit 1
fi
