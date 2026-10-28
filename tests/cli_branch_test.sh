#!/usr/bin/env bash
# Compare native discovery with Git; no inference, sleeps or external services.
set -euo pipefail
tester=$(realpath "$1")
root=$(mktemp -d "$(dirname "$tester")/branch-test.XXXXXX")
trap 'rm -rf "$root"' EXIT
export HOME="$root" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null LC_ALL=C
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE GIT_CONFIG_COUNT
export GIT_CEILING_DIRECTORIES="$root"
native() { PATH=/nonexistent "$tester" --branch "$1"; }
compare() {
  local expected actual
  expected=$(git -C "$1" symbolic-ref --quiet --short HEAD 2>/dev/null || :)
  actual=$(native "$1")
  if [[ "$actual" != "$expected" ]]; then
    printf 'Branch mismatch in %q: native=%q git=%q\n' "$1" "$actual" "$expected" >&2
    exit 1
  fi
}
mkdir "$root/plain"
compare "$root/plain"
compare "$root/nonexistent"
git init -q -b main --template= "$root/repo"
mkdir -p "$root/repo/a/b/c"
compare "$root/repo"; compare "$root/repo/a/b/c"
git -C "$root/repo" symbolic-ref HEAD refs/heads/feature/changed
compare "$root/repo/a/b/c"
git -C "$root/repo" -c core.hooksPath=/dev/null -c commit.gpgsign=false \
  -c user.name=Fixture -c user.email=fixture@example.invalid commit -q --allow-empty -m fixture
git -C "$root/repo" worktree add -q -b feature/linked "$root/linked"
mkdir -p "$root/linked/deep/inside"
compare "$root/linked/deep/inside"
target=$(sed 's/^gitdir: //' "$root/linked/.git")
printf 'gitdir: %s\n' "$(realpath --relative-to="$root/linked" "$target")" >"$root/linked/.git"
compare "$root/linked/deep/inside"
git init -q -b separate --template= --separate-git-dir="$root/metadata" "$root/separate"
compare "$root/separate"
git init -q -b bare --template= --bare "$root/bare"
compare "$root/bare"
git init -q -b inner --template= "$root/repo/a/inner"
compare "$root/repo/a/inner"
ln -s "$root/repo/a/b" "$root/alias"
compare "$root/alias"
GIT_DIR="$root/metadata" compare "$root/plain"
GIT_DIR=../metadata compare "$root/plain"
git -C "$root/repo" checkout -q --detach
compare "$root/repo/a/b/c"
mv "$root/repo/.git/HEAD" "$root/head.saved"
compare "$root/repo"
mv "$root/head.saved" "$root/repo/.git/HEAD"
chmod 000 "$root/repo/.git/HEAD"
compare "$root/repo"
chmod 600 "$root/repo/.git/HEAD"
mkdir "$root/broken"
printf 'gitdir: /nonexistent/git-dir\n' >"$root/broken/.git"
compare "$root/broken"
printf 'not a gitfile\n' >"$root/broken/.git"
compare "$root/broken"
# Legitimate Markdown-looking text remains literal, ordinary Unicode survives.
for name in 'feature/<tag>`literal`_text_' 'feature/λ'; do
  git -C "$root/repo" symbolic-ref HEAD "refs/heads/$name"
  compare "$root/repo/a/b/c"
done
for pair in $'feature/\342\200\256evil|feature/\\u202eevil' \
            $'feature/\302\233evil|feature/\\u009bevil' \
            $'feature/\342\200\250evil|feature/\\u2028evil'; do
  name=${pair%|*}; safe=${pair#*|}
  git -C "$root/repo" symbolic-ref HEAD "refs/heads/$name"
  [[ $(git -C "$root/repo" symbolic-ref --short HEAD) == "$name" ]]
  [[ $(native "$root/repo") == "$safe" ]]
done
printf 'ref: refs/heads/invalid\377\n' >"$root/repo/.git/HEAD"
[[ $(native "$root/repo") == 'invalid?' ]]
for bad in $'ref: refs/heads/escape\033[31m' $'ref: refs/heads/a\nforged'; do
  printf '%s\n' "$bad" >"$root/repo/.git/HEAD"
  [[ -z $(native "$root/repo") ]]
done
# Embedded NUL must be written directly; shell variables cannot contain it.
printf 'ref: refs/heads/a\000ignored\n' >"$root/repo/.git/HEAD"
[[ -z $(native "$root/repo") ]]
name=feature/; expected=feature/
for ((i=0; i<100; i++)); do name+=λ; ((i>=75)) || expected+=λ; done
git -C "$root/repo" symbolic-ref HEAD "refs/heads/$name"
[[ $(git -C "$root/repo" symbolic-ref --short HEAD) == "$name" ]]
[[ $(native "$root/repo") == "$expected" ]]
rm "$root/repo/.git/HEAD"
mkfifo "$root/repo/.git/HEAD"
[[ -z $(native "$root/repo") ]]
git init -q -b main --template= "$root/debounce"
PATH=/nonexistent "$tester" --debounce "$root/debounce"
