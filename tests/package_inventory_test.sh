#!/usr/bin/env bash
set -euo pipefail
repo=$1; scratch=$(mktemp -d "$2/package-inventory.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/dist"
version=1.2.3
names=(cai-1.2.3-x86_64-linux-gnu.tar.gz cai-1.2.3.tar.gz cai-lua-1.2.3.tar.gz cai-1.2.3-1.rockspec cai-1.2.3-1.src.rock)
run() { cmake "-DCAI_DIST_DIR=$scratch/dist" "-DCAI_VERSION=$version" -DCAI_TARGETS=x86_64-linux-gnu "$@" -P "$repo/cmake/package_checksums.cmake"; }
for name in "${names[@]}"; do printf 'fixture %s\n' "$name" >"$scratch/dist/$name"; done
# Incremental preparation can retain a different version with the same prefix.
printf '%s\n' obsolete >"$scratch/dist/cai-$version-flow.1-x86_64-linux-gnu.tar.gz"
printf '%s\n' obsolete >"$scratch/dist/cai-$version-flow.1-1.src.rock"
run
[[ $(wc -l <"$scratch/dist/cai-$version-CHECKSUMS") == ${#names[@]} ]]
(cd "$scratch/dist" && sha256sum -c "cai-$version-CHECKSUMS")
cp "$scratch/dist/cai-$version-CHECKSUMS" "$scratch/manifest"
for argument in '-DCAI_TARGETS=' '-DCAI_TARGETS=unknown' '-DCAI_PACKAGE_SCOPE=unknown'; do
  if run "$argument" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
done
for path in '' "$scratch/dist/cai-$version-CHECKSUMS"; do
  if run -DCAI_PACKAGE_SCOPE=binary "-DCAI_CHECKSUMS_PATH=$path" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
done
ln -s "$scratch/dist/cai-$version-CHECKSUMS" "$scratch/manifest-alias"
if run -DCAI_PACKAGE_SCOPE=binary "-DCAI_CHECKSUMS_PATH=$scratch/manifest-alias" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
run -DCAI_PACKAGE_SCOPE=binary "-DCAI_CHECKSUMS_PATH=$scratch/binary-CHECKSUMS"
[[ $(wc -l <"$scratch/binary-CHECKSUMS") == 1 ]]
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
rm "$scratch/dist/${names[0]}"
if run >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
cmp "$scratch/manifest" "$scratch/dist/cai-$version-CHECKSUMS"
grep -q 'Missing/nonregular selected artifact' "$scratch/err"
printf 'fixture %s\n' "${names[0]}" >"$scratch/dist/${names[0]}"
# The final verifier rejects a checksum-valid but incomplete inventory.
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
export CAI_RELEASE_TARGETS=x86_64-linux-gnu
dist_dir="$scratch/dist"
verify_manifest_artifacts "${names[@]}"
require_complete_manifest "${names[@]}"
if (require_complete_manifest "cai-$version-flow.1-x86_64-linux-gnu.tar.gz" "${names[@]:1}") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
if (require_complete_manifest "${names[@]:1}") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi )
# All six Linux SDKs must ship the static/shared app surface.
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
verify_application_sdk "$scratch/sdk" arm64-apple-darwin
paths=(include/cai/app.h lib/libcai_app.a lib/libcai_app.so
  lib/pkgconfig/cai-app.pc lib/cmake/cai_app/cai_app-config.cmake
  share/cai_app/dependencies.json share/doc/libcai_app/embedding.md
  share/doc/libcai_app/examples/main.c share/doc/libcai_app/examples/app.lua)
for path in "${paths[@]}"; do mkdir -p "$scratch/sdk/$(dirname "$path")"; touch "$scratch/sdk/$path"; done
for target in x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl; do
  verify_application_sdk "$scratch/sdk" "$target"
  for path in "${paths[@]}"; do
    mv "$scratch/sdk/$path" "$scratch/missing"
    if (verify_application_sdk "$scratch/sdk" "$target") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
    grep -Fq "missing required file: $scratch/sdk/$path" "$scratch/err"
    mv "$scratch/missing" "$scratch/sdk/$path"
  done
done )

# Every selected musl SDK requires its matching standalone CLI. GNU SDKs do not.
selected=x86_64-linux-gnu
for arch in x86_64 aarch64 armhf; do
  target=$arch-linux-musl
  selected+=";$target"
  names+=("cai-$version-$target.tar.gz" "cai-cli-$version-$target.tar.gz")
done
for name in "${names[@]}"; do printf 'fixture %s\n' "$name" >"$scratch/dist/$name"; done
run "-DCAI_TARGETS=$selected"
[[ $(wc -l <"$scratch/dist/cai-$version-CHECKSUMS") == ${#names[@]} ]]
cp "$scratch/dist/cai-$version-CHECKSUMS" "$scratch/complete-manifest"
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
export CAI_RELEASE_TARGETS="$selected"
dist_dir="$scratch/dist"
verify_manifest_artifacts "${names[@]}"
require_complete_manifest "${names[@]}"
for arch in x86_64 aarch64 armhf; do
  missing="cai-cli-$version-$arch-linux-musl.tar.gz"
  remaining=()
  for name in "${names[@]}"; do [[ "$name" == "$missing" ]] || remaining+=("$name"); done
  if (require_complete_manifest "${remaining[@]}") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
done )
for arch in x86_64 aarch64 armhf; do
  missing="cai-cli-$version-$arch-linux-musl.tar.gz"
  mv "$scratch/dist/$missing" "$scratch/missing"
  if run "-DCAI_TARGETS=$selected" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  cmp "$scratch/complete-manifest" "$scratch/dist/cai-$version-CHECKSUMS"
  mv "$scratch/missing" "$scratch/dist/$missing"
done

# CLI selector validation precedes deleting staging or replacing an archive.
mkdir -p "$scratch/cli-build/package-cli-stage"
printf keep >"$scratch/cli-build/package-cli-stage/sentinel"
for target in '' x86_64-linux-gnu arm64-apple-darwin unknown-linux-musl; do
  if cmake "-DCAI_BINARY_DIR=$scratch/cli-build" "-DCAI_DIST_DIR=$scratch/dist" \
      "-DCAI_VERSION=$version" "-DCAI_TARGET_ID=$target" -P "$repo/cmake/package_cli.cmake" \
      >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'supported Linux musl CLI target' "$scratch/err"
  [[ $(cat "$scratch/cli-build/package-cli-stage/sentinel") == keep ]]
done

# ELF metadata fixtures isolate wrong architecture/linkage from inference or
# foreign execution. The actual cross artifacts are checked separately.
cat >"$scratch/readelf" <<'SH'
#!/usr/bin/env bash
[[ ${CAI_ELF_FAIL:-0} == 0 ]] || exit 7
case "$1" in
  -h) cat "$CAI_ELF_FIXTURE/header" ;;
  -l) cat "$CAI_ELF_FIXTURE/segments" ;;
  *) exit 2 ;;
esac
SH
touch "$scratch/cai"
chmod +x "$scratch/readelf" "$scratch/cai"
( CAI_REPO_ROOT="$repo" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
export CAI_ELF_FIXTURE="$scratch"
find_target_readelf() { printf '%s\n' "$scratch/readelf"; }
for arch in x86_64 aarch64 armhf; do
  case "$arch" in
    x86_64) elf_class=ELF64; machine='Advanced Micro Devices X86-64' ;;
    aarch64) elf_class=ELF64; machine=AArch64 ;;
    armhf) elf_class=ELF32; machine=ARM ;;
  esac
  printf 'Class: %s\nMachine: %s\nFlags: hard-float ABI\n' "$elf_class" "$machine" >"$scratch/header"
  printf 'LOAD\n' >"$scratch/segments"
  verify_cli_binary "$scratch/cai" "$arch-linux-musl"
  cp "$scratch/header" "$scratch/good-header"
  for field in class machine float; do
    [[ "$field" != float || "$arch" == armhf ]] || continue
    case "$field" in
      class) sed 's/Class:.*/Class: ELF00/' "$scratch/good-header" >"$scratch/header" ;;
      machine) sed 's/Machine:.*/Machine: Other/' "$scratch/good-header" >"$scratch/header" ;;
      float) sed 's/hard-float/soft-float/' "$scratch/good-header" >"$scratch/header" ;;
    esac
    if (verify_cli_binary "$scratch/cai" "$arch-linux-musl") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  done
  cp "$scratch/good-header" "$scratch/header"
  for segment in INTERP DYNAMIC; do
    printf '  %s 0x0\n' "$segment" >"$scratch/segments"
    if (verify_cli_binary "$scratch/cai" "$arch-linux-musl") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
    grep -q 'statically linked' "$scratch/err"
  done
  printf 'LOAD\n' >"$scratch/segments"
  if (CAI_ELF_FAIL=1 verify_cli_binary "$scratch/cai" "$arch-linux-musl") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
done
if (verify_cli_binary "$scratch/cai" x86_64-linux-gnu) >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
chmod -x "$scratch/cai"
if (verify_cli_binary "$scratch/cai" armhf-linux-musl) >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
)
