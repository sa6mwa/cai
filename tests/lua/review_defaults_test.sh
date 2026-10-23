#!/usr/bin/env bash
source "$(dirname "$0")/../cli_test_common.sh"
runner=$1
peer_start
git -C "$work" init -q -b trunk
git -C "$work" config core.hooksPath /dev/null
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(review): seed Lua base' --allow-empty
report=$(jq -c . "$support/fixtures/review.json")
plan "$(jq -n --arg report "$report" '{plan:[$report,$report,$report,$report,$report]}')"
export CAI_TEST_REVIEW_ENDPOINT="$endpoint"
"$runner" "$support/lua/review_defaults.lua" "$work"
[[ $(requests) == 5 ]]
head -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" "base branch 'refs/heads/trunk'"
sed -n '2p' "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'staged, unstaged, and untracked'
sed -n '3p' "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" "base branch 'HEAD'"
sed -n '4p' "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'Custom Lua review scope'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" "base branch 'refs/heads/trunk'"
