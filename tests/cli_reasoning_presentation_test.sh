#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":[{"reasoning":["**Applying ","patch to script path**"],"pause":true,"text":"ONE LINE DONE"}]}'
terminal_start -N
send 'first\r'; expect 'STREAM in flight'
absent "$term/text" '[reasoning]'
absent "$term/text" '? Applying patch'
# Feed text can arrive before the independently refreshed status line.
expect '! Applying patch to script path'
release; expect 'ONE LINE DONE'; expect 'Worked for'
terminal_close
# A failed tool and its diagnostic must begin outside the reasoning quote,
# including when the final reasoning fragment has no terminating newline.
plan '{"plan":[{"reasoning":["**Checking goal**\n\nREASONING ENDS HERE"],"tool":"create_goal","arguments":{"unexpected":true}},"AFTER TOOL FAILURE"]}'
terminal_start -N
prompt 'check reasoning-to-tool boundary' 'AFTER TOOL FAILURE'
sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' "$term/feed" >"$root/failed-tool"
grep -q '^\* Failed create_goal' "$root/failed-tool"
grep -q '^\* WARN:' "$root/failed-tool"
if grep -Eq '^.*[>│].*\* (Failed|WARN:)' "$root/failed-tool"; then fail 'tool output retained a reasoning quote'; fi
terminal_close
plan '{"plan":[{"reasoning_parts":[["**Planning full ANSI Tetris implementation**"],["**Confirming ","build and source setup**"]],"pause":true,"text":"PARTS DONE"}]}'
terminal_start -N
send 'separate reasoning sections\r'; expect 'STREAM in flight'
expect '! Confirming build and source setup'
absent "$term/text" 'implementationConfirming'
absent "$term/text" '? Confirming'
release; expect 'PARTS DONE'; expect 'Worked for'
terminal_close
plan '{"plan":[{"reasoning":["**Checking the change**\r","\n\n**Body line one.**\n\n","Body line two with `code`."],"pause":true,"text":"MULTILINE DONE"}]}'
terminal_start -N
send 'second\r'; expect 'STREAM in flight'
expect '! Checking the change'
contains "$term/feed" '  > Body line one.'
grep -Eq $'\033\\[(0;)?90m>' "$term/raw"
contains "$term/text" 'Body line one.'
absent "$term/text" '[reasoning]'
release; expect 'MULTILINE DONE'; expect 'Worked for'
terminal_close
# Commentary, a provider summary and more answer text can share one response.
# The paused stream is still an active turn; only visible block types change.
plan '{"plan":[{"preamble":"Commentary before reasoning.","reasoning":["**Choosing a boundary**\n\nSummary body ","one continues.\n\nSummary body two."],"pause":true,"pause_text":"Answer after reasoning. ","text":"Still the same response."}]}'
terminal_start -N
before=$(requests)
send 'alternate text and reasoning\r'
expect 'Answer after reasoning.'
expect '! Choosing a boundary'
absent "$term/text" 'Worked for'
release; expect 'Still the same response.'; expect 'Worked for'
[[ $(requests) == $((before+1)) ]]
sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' "$term/feed" >"$root/blocks"
contains "$root/blocks" $'  Commentary before reasoning.\n\n  > Summary body one continues.'
contains "$root/blocks" $'  > Summary body two.\n\n  Answer after reasoning. Still the same response.'
absent "$root/blocks" 'Choosing a boundary'
absent "$root/blocks" '? Answer after reasoning.'
terminal_close
# Wrapping belongs to libmdf, including the first row, continuations, CRLF
# across deltas and a final body line without a terminating newline.
width=60
plan '{"plan":[{"reasoning":["**Quoted plan**\n\nFIRST quoted paragraph is deliberately long enough to wrap across multiple rows while preserving **Markdown** and `code`.","\r","\n\r\nSECOND quoted paragraph is also long enough to wrap, ending at ENDREASON"],"pause":true,"pause_text":"ANSWER after quote. ","text":"QUOTE DONE"}]}'
terminal_start -N
send 'wrap the summary\r'; expect 'ANSWER after quote.'; expect '! Quoted plan'
sed -E $'s/\033\[[0-?]*[ -/]*[@-~]//g' "$term/feed" >"$root/wrapped"
awk '
  /^  > FIRST/ {inside=1}
  /^  ANSWER after quote/ {inside=0; answer++}
  inside && NF {if ($0 !~ /^  >/) exit 1; rows++}
  END {if (rows<4 || answer!=1) exit 1}
' "$root/wrapped"
contains "$root/wrapped" ENDREASON
absent "$root/wrapped" '? '
grep -Eq $'\033\\[(0;)?90m>' "$term/raw"
release; expect 'QUOTE DONE'; expect 'Worked for'
terminal_close
plan '{"plan":[{"reasoning_parts":[["**First heading**\n\nFirst section body."],["**Second heading**\n\nSecond section body."]],"pause":true,"text":"SECTIONS DONE"}]}'
terminal_start -N
send 'separate quoted sections\r'; expect '! Second heading'; expect 'STREAM in flight'
contains "$term/feed" $'  > First section body.\n\n  > Second section body.'
absent "$term/feed" 'First heading'
absent "$term/feed" 'Second heading'
release; expect 'SECTIONS DONE'; expect 'Worked for'
terminal_close
