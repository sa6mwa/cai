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
contains "$term/text" '? '
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
contains "$root/blocks" $'  Commentary before reasoning.\n\n? Summary body one continues.'
contains "$root/blocks" $'? Summary body two.\n\n  Answer after reasoning. Still the same response.'
absent "$root/blocks" 'Choosing a boundary'
absent "$root/blocks" '? Answer after reasoning.'
terminal_close
