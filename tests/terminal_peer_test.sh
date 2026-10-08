#!/usr/bin/env bash
# Native PTY data fixture: inspect prompt snapshots, not flattened redraw bytes.
set -euo pipefail
peer=$1
scratch=$(mktemp -d "${TMPDIR:-$(dirname "$peer")}/terminal-peer.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/preserve" "$scratch/erase"
cat >"$scratch/preserve.sh" <<'SH'
printf '\033[1;27r\033[65535;1H> preserv'
printf '\033[65535;1H\033[1A\033[0KMODEL BAR'
printf '\033[65535;10Hed-draft'
printf '\033[65535;1H\033[2A\033[0K! Working'
printf '\033[65535;18H'
SH
"$peer" "$scratch/preserve" 120 /bin/sh "$scratch/preserve.sh" </dev/null
[[ $(cat "$scratch/preserve/prompt") == '> preserved-draft' ]]
! grep -Fq preserved-draft "$scratch/preserve/text"
cat >"$scratch/erase.sh" <<'SH'
printf '\033[65535;1H> previous-draft'
printf '\033[65535;1H\033[2K> history-steering'
printf '\033[65535;1H\033[1A\033[0KSTATUS'
printf '\033[65535;20H'
SH
"$peer" "$scratch/erase" 120 /bin/sh "$scratch/erase.sh" </dev/null
[[ $(cat "$scratch/erase/prompt") == '> history-steering' ]]
