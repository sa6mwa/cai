#!/usr/bin/env bash
# Inject metadata-tool failures; never create or mutate repository refs.
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/release-metadata.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "release metadata regression failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; cat "$scratch/err" >&2 2>/dev/null || true' ERR
mkdir -p "$scratch/bin" "$scratch/metadata-fixture/build"
metadata_root="$scratch/metadata-fixture"
cp "$repo/README.md" "$metadata_root/"
export CAI_METADATA_ROOT="$metadata_root"
cat >"$scratch/bin/git" <<'SH'
#!/usr/bin/env bash
for argument in "$@"; do
  if [[ "$argument" == "${CAI_METADATA_FAIL:-}" ]]; then
    printf 'fixture: Git %s failed\n' "$argument" >&2
    exit 128
  fi
done
case "${3:-}:${4:-}" in
  rev-parse:--show-toplevel) printf '%s\n' "$CAI_METADATA_ROOT" ;;
  rev-parse:HEAD) printf '0123456789012345678901234567890123456789\n' ;;
  for-each-ref:*) ;;
  ls-files:--deleted) ;;
  ls-files:*) printf 'README.md\n' ;;
  check-ignore:*) exit 1 ;;
  *) exit 2 ;;
esac
SH
chmod +x "$scratch/bin/git"
export PATH="$scratch/bin:$PATH" CAI_VERSION_OVERRIDE= CAI_METADATA_FAIL=
if CAI_METADATA_FAIL=for-each-ref bash "$repo/scripts/release_version.sh" "$metadata_root" >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
grep -q 'cannot read release tags' "$scratch/err"
[[ ! -s "$scratch/out" ]]
# A declared candidate override deliberately does not need the tag catalog.
[[ $(CAI_VERSION_OVERRIDE=1.2.3 CAI_METADATA_FAIL=for-each-ref bash "$repo/scripts/release_version.sh" "$metadata_root") == 1.2.3 ]]
[[ $(bash "$repo/scripts/release_version.sh" "$metadata_root") == 0.0.0 ]]
if CAI_METADATA_FAIL=check-ignore bash "$repo/scripts/stage_release_sources.sh" "$metadata_root" "$metadata_root/build/stage" 1.2.3 >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
grep -q 'failed to classify ignored source paths' "$scratch/err"
[[ ! -e "$metadata_root/build/stage/VERSION" && ! -e "$metadata_root/build/stage/RELEASE_MANIFEST" ]]
bash "$repo/scripts/stage_release_sources.sh" "$metadata_root" "$metadata_root/build/stage" 1.2.3
cmp "$metadata_root/README.md" "$metadata_root/build/stage/README.md"
# Missing and empty declared header inputs must fail before replacing staging.
fixture="$scratch/lua-fixture"
mkdir -p "$fixture/build/stage" "$fixture/include"
printf retained >"$fixture/build/stage/sentinel"
for mode in missing empty; do
  [[ "$mode" != empty ]] || mkdir "$fixture/include/cai"
  if bash "$repo/scripts/stage_lua_rock_sources.sh" "$fixture" "$fixture/build/stage" 1.2.3 >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'public header' "$scratch/err"
  [[ $(cat "$fixture/build/stage/sentinel") == retained ]]
done
