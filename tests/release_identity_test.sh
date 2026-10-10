#!/usr/bin/env bash
# Real signatures and tag shapes, confined to this disposable fixture.
set -Eeuo pipefail
repo=$1
export CAI_VERSION_OVERRIDE= RELEASE_VERSION=
scratch=$(mktemp -d "${TMPDIR:-$repo/build}/release-identity.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'printf "release identity fixture failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2; cat "$scratch/out" "$scratch/err" >&2 2>/dev/null || true' ERR
fixture="$scratch/checkout with spaces"
mkdir -p "$fixture/scripts" "$fixture/build" "$fixture/dist" "$fixture/.cache"
cp "$repo/scripts/"{release_version,release}.sh "$fixture/scripts/"
cp "$repo/Makefile" "$fixture/"
git -C "$fixture" init -q
git -C "$fixture" config user.name Fixture
git -C "$fixture" config user.email fixture@example.invalid
git -C "$fixture" config core.hooksPath /dev/null
printf 'build/\ndist/\n.cache/\n' >"$fixture/.gitignore"
git -C "$fixture" add .
git -C "$fixture" -c commit.gpgSign=false commit -qm 'test(release): unsigned candidate'
git -C "$fixture" -c tag.gpgSign=false tag v1.2.3
for directory in build dist .cache; do printf retained >"$fixture/$directory/sentinel"; done
reject() {
  if bash "$fixture/scripts/release.sh" final >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
  grep -Fq "$1" "$scratch/err"
  for directory in build dist .cache; do [[ -f "$fixture/$directory/sentinel" ]]; done
}
reject 'verified signature'
# Generate an isolated SSH signing key, never use or modify account credentials.
ssh-keygen -q -t ed25519 -N '' -f "$scratch/signing-key"
printf 'fixture@example.invalid %s\n' "$(cat "$scratch/signing-key.pub")" >"$scratch/allowed-signers"
git -C "$fixture" config gpg.format ssh
git -C "$fixture" config user.signingkey "$scratch/signing-key"
git -C "$fixture" config gpg.ssh.allowedSignersFile "$scratch/allowed-signers"
git -C "$fixture" -c commit.gpgSign=true commit --amend --no-edit -q
# An amendment invalidates the old tag identity; it must be frozen again.
reject 'exact lightweight'
git -C "$fixture" tag -d v1.2.3 >/dev/null
git -C "$fixture" -c tag.gpgSign=false tag v1.2.3
[[ $(bash "$fixture/scripts/release_version.sh" "$fixture" --require-release) == 1.2.3 ]]
if make --no-print-directory -C "$fixture" release RELEASE_VERSION=9.9.9 >"$scratch/out" 2>"$scratch/err"; then exit 1; fi
grep -Fq 'forbids RELEASE_VERSION overrides' "$scratch/err"
for directory in build dist .cache; do [[ -f "$fixture/$directory/sentinel" ]]; done
printf dirty >>"$fixture/.gitignore"
reject 'clean committed'
git -C "$fixture" restore .gitignore
: >"$scratch/allowed-signers"
reject 'verified signature'
printf 'fixture@example.invalid %s\n' "$(cat "$scratch/signing-key.pub")" >"$scratch/allowed-signers"
git -C "$fixture" tag -d v1.2.3 >/dev/null
git -C "$fixture" -c tag.gpgSign=false tag -a v1.2.3 -m 'fixture annotated tag'
reject 'not lightweight'
git -C "$fixture" tag -d v1.2.3 >/dev/null
git -C "$fixture" -c tag.gpgSign=false tag candidate
# A symbolic tag pointing to a commit does not establish lightweight identity.
git -C "$fixture" symbolic-ref refs/tags/v1.2.3 refs/tags/candidate
reject 'symbolic'
