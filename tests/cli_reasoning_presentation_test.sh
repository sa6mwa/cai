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
plan '{"plan":[{"reasoning":["**Checking the change**\r","\n\n**Body line one.**\n\n","Body line two with `code`."],"pause":true,"text":"MULTILINE DONE"}]}'
terminal_start -N
send 'second\r'; expect 'STREAM in flight'
expect '! Checking the change'
contains "$term/text" '? '
contains "$term/text" 'Body line one.'
absent "$term/text" '[reasoning]'
release; expect 'MULTILINE DONE'; expect 'Worked for'
terminal_close
