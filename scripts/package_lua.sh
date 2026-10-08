#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
version=${1:?release version required}
cd "$root"
staging="$root/build/lua-package"
mkdir -p "$root/build" "$root/dist"
rm -rf "$staging"; mkdir -p "$staging/cai-$version"
trap 'rm -rf "$staging"' EXIT
"$root/scripts/stage_lua_rock_sources.sh" "$root" "$staging/cai-$version" "$version"
# Normalize directories, preserve the manifest's intentional executable files.
find "$staging" -type d -exec chmod 755 {} +
tar --sort=name --mtime=@0 --mode=u+rw,go+r,go-w,a+X --format=gnu --owner=0 --group=0 -C "$staging" -cf "$staging/cai-lua-$version.tar" "cai-$version"
gzip -9 -f -n "$staging/cai-lua-$version.tar"
mv "$staging/cai-lua-$version.tar.gz" "$root/dist/"
extension=$(luarocks config variables.LIB_EXTENSION)
"$root/scripts/render_release_rockspec.sh" "$version" "$root/dist/cai-$version-1.rockspec" "file://cai-lua-$version.tar.gz" "" "$extension" "cai-$version"
cp "$root/dist/cai-$version-1.rockspec" "$root/dist/cai-lua-$version.tar.gz" "$staging/"
chmod 644 "$staging/cai-$version-1.rockspec" "$staging/cai-lua-$version.tar.gz"
TZ=UTC touch -t 198001010000.00 "$staging/cai-$version-1.rockspec" "$staging/cai-lua-$version.tar.gz"
rm -f "$root/dist/cai-$version-1.src.rock"
(cd "$staging" && zip -X -q "$root/dist/cai-$version-1.src.rock" "cai-$version-1.rockspec" "cai-lua-$version.tar.gz")
