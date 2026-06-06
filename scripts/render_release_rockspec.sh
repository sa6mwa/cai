#!/usr/bin/env bash
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 6 ]; then
  printf '%s\n' "usage: $0 VERSION OUTPUT_PATH [SOURCE_URL] [SOURCE_TAG] [LIB_EXTENSION] [SOURCE_DIR]" >&2
  exit 1
fi

version="$1"
output_path="$2"
source_url="git+https://github.com/sa6mwa/cai.git"
source_tag=""
tag_line=""
dir_line=""
lib_extension="so"

if [ "$#" -ge 3 ]; then
  source_url="$3"
fi

if [ "$#" -ge 4 ]; then
  source_tag="$4"
elif [ "$version" != "0.0.0" ]; then
  source_tag="v$version"
fi

if [ "$#" -ge 5 ]; then
  lib_extension="$5"
fi

if [ "$#" -ge 6 ] && [ -n "$6" ]; then
  dir_line='  dir = "'"$6"'",'
fi

if [ -n "$source_tag" ]; then
  tag_line='  tag = "'"$source_tag"'",'
fi

sed \
  -e "s|@VERSION@|$version|g" \
  -e "s|@SOURCE_URL@|$source_url|g" \
  -e "s|@SOURCE_TAG_LINE@|$tag_line|g" \
  -e "s|@SOURCE_DIR_LINE@|$dir_line|g" \
  -e "s|@LIB_EXTENSION@|$lib_extension|g" \
  cai.rockspec.in >"$output_path"
