#!/usr/bin/env bash
# Exercise released source and Lua bytes without compiling a product or fetching.
set -euo pipefail
repo=$1
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/release-packaging.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "packaging regression failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR
fixture="$scratch/source"
mkdir -p "$fixture/scripts" "$fixture/cmake" "$fixture/lua" "$fixture/include" "$fixture/docs" "$fixture/examples" "$fixture/cli"
cp "$repo/"{LICENSE,README.md,cai.rockspec.in} "$fixture/"
cp "$repo/docs/model-metadata.md" "$fixture/docs/"
cp "$repo/docs/embedding.md" "$fixture/docs/"
cp "$repo/cli/main.c" "$fixture/cli/"
cp "$repo/examples/app.lua" "$fixture/examples/"
cp "$repo/scripts/"{package_lua,stage_lua_rock_sources,stage_release_sources,render_release_rockspec,build_lua_rock}.sh "$fixture/scripts/"
mv "$fixture/scripts/stage_release_sources.sh" "$fixture/scripts/stage_release_sources_real.sh"
cat >"$fixture/scripts/stage_release_sources.sh" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$2" >"$1/build/observed-source-stage"
exec "$(dirname "$0")/stage_release_sources_real.sh" "$@"
SH
chmod +x "$fixture/scripts/stage_release_sources.sh"
cp "$repo/cmake/cai-export-policy.cmake" "$fixture/cmake/"
cp "$repo/lua/"{CMakeLists.txt,cai_lua.c,cai_lua_config.inc,cai_lua_skills.inc,cai_lua.exports,app_lua.c,cai_app.exports} "$fixture/lua/"
cp -r "$repo/include/cai" "$fixture/include/"
(cd "$fixture" && find . -type f -print | sed 's|^./||' | sort >RELEASE_MANIFEST)
version=1.2.3
cmake "-DCAI_ROOT=$fixture" "-DCAI_VERSION=$version" -P "$repo/cmake/package_source.cmake"
[[ $(cat "$fixture/build/observed-source-stage") == "$fixture/build/source-pack/cai-$version" ]]
[[ -f "$fixture/dist/cai-$version.tar.gz" && ! -e "$fixture/dist/.source-pack" && ! -e "$fixture/build/source-pack" ]]
mkdir "$scratch/source-extract"
tar -xzf "$fixture/dist/cai-$version.tar.gz" -C "$scratch/source-extract"
[[ $(cat "$scratch/source-extract/cai-$version/VERSION") == "$version" ]]
cmp "$fixture/lua/cai_lua.c" "$scratch/source-extract/cai-$version/lua/cai_lua.c"
grep -qx VERSION "$scratch/source-extract/cai-$version/RELEASE_MANIFEST"
bash "$fixture/scripts/package_lua.sh" "$version"
rockspec="$fixture/dist/cai-$version-1.rockspec"
rock="$fixture/dist/cai-$version-1.src.rock"
grep -Fq "https://github.com/sa6mwa/cai/releases/download/v$version/cai-lua-$version.tar.gz" "$rockspec"
luarocks lint "$rockspec"
# A public source URL still permits offline unpacking of the embedded source rock.
mkdir "$scratch/unpack"
(cd "$scratch/unpack" && luarocks unpack "$rock")
cmp "$fixture/lua/cai_lua.c" "$scratch/unpack/cai-$version-1/cai-$version/lua/cai_lua.c"
[[ ! -e "$fixture/build/lua-package" ]]
(
  CAI_REPO_ROOT="$fixture" CAI_VERSION="$version" source "$repo/scripts/verify_release_artifacts.sh" --self-test
  verify_rockspec_file "$rockspec"
  verify_src_rock "$rock"
  sed "s|https://github.com/sa6mwa/cai/releases/download/v$version/|file://|" "$rockspec" >"$scratch/private.rockspec"
  if (verify_rockspec_file "$scratch/private.rockspec") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'rockspec contains host-specific' "$scratch/err"
  mkdir "$scratch/private-rock"
  cp "$scratch/private.rockspec" "$scratch/private-rock/cai-$version-1.rockspec"
  (cd "$scratch/private-rock" && zip -X -q "$rock" "cai-$version-1.rockspec")
  if (verify_src_rock "$rock") >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -q 'rockspec contains host-specific' "$scratch/err"
)
