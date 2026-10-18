#!/usr/bin/env bash
# Native PTY data fixture: inspect prompt snapshots, not flattened redraw bytes.
set -euo pipefail
peer=$1
scratch=$(mktemp -d "${TMPDIR:-$(dirname "$peer")}/terminal-peer.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/preserve" "$scratch/erase" "$scratch/feed" "$scratch/query"
cat >"$scratch/query.sh" <<'SH'
set -euo pipefail
stty -echo -icanon min 1 time 0
printf '\033[6n'
IFS= read -r -d R -t 2 reply
[[ "$reply" == $'\033[1;1' ]]
printf '\033[12;37H\033[6n'
IFS= read -r -d R -t 2 reply
[[ "$reply" == $'\033[12;37' ]]
printf 'QUERY_REPLIED'
SH
mkfifo "$scratch/query/events"
exec 8<>"$scratch/query/events"
CAI_TEST_TERMINAL_EVENT_FD=8 "$peer" "$scratch/query" 120 /bin/bash "$scratch/query.sh" </dev/null
IFS= read -r -N 1 -t .1 -u 8 ready
[[ "$ready" == . ]]
exec 8>&-
grep -Fq QUERY_REPLIED "$scratch/query/text"
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
cat >"$scratch/feed.sh" <<'SH'
printf '\033[1;27r\033[65535;3H\033[3ARESPONSE alpha'
printf '\033[65535;1H\033[2A! Working'
printf '\033[65535;17H\033[3A beta\r\n'
printf '\033[65535;1H\033[2A! Worked for 1s'
printf '\033[65535;1H\033[2K> draft'
SH
"$peer" "$scratch/feed" 120 /bin/sh "$scratch/feed.sh" </dev/null
[[ $(cat "$scratch/feed/feed") == 'RESPONSE alpha beta' ]]
[[ $(cat "$scratch/feed/prompt") == '> draft' ]]
! grep -Fq 'RESPONSE alpha beta' "$scratch/feed/text"
