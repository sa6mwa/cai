#!/usr/bin/env bash
set -euo pipefail
repo=$1
# Live coverage is explicitly selected per run; no receipt hash or stale success stamp.
! grep -q 'RELEASE_LIVE_GATE_STAMP\|worktree-status-sha\|require-prerelease-live' "$repo/Makefile"
grep -q 'CAI_ENABLE_INTEGRATION_TESTS' "$repo/Makefile"
grep -q 'prerelease-live: test-integration' "$repo/Makefile"
