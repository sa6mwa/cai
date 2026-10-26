# Softline 0.10.0: retained-frame resize corrupts a subsequent heading

## Status

**Resolved for the controlled sequence (2026-10-19):** CAI calls
`sl_output_stream_reconcile()` on the owner thread before
resuming output after changed terminal dimensions. The retained/shrink regression
is enabled and passes, alongside the other seven terminal resize scenarios.
CAI now pins softline 0.13.0, which retains the 0.11.0 reconciliation API and
adds further growth/shrink and retained-cursor corrections.
The historical diagnosis and captures below describe the 0.10.0 baseline.

Reproduced twice with a softline-only C program on 2026-10-16, Linux x86_64,
tmux 3.6, softline 0.10.0. The producer emits only ASCII, LF and valid ANSI SGR.
The standalone program uses no libcai, libmdf, provider, tool runtime or Pouch.

This demonstrates the same form of stale-cell corruption reported in CAI.
The original incident used xfce4-terminal on local X without tmux/SSH; this
investigation cannot establish its exact sequence or exclude another trigger.

The original failing path was covered by softline's documented **Review exception:
unobserved retained-frame resize**, rather than a newly discovered violation
of its guaranteed input-owned resize behavior. Version 0.11.0 supplies the
explicit owner reconciliation needed for command submission and session replay.

## Build and reproduce

From the CAI checkout, with its normal prepared native toolchain/dependencies
and tmux available:

```sh
cmake --preset debug-cai
cmake --build --preset debug-cai --target cai_cli_softline_resize_repro
bash tests/softline_resize_repro.sh \
  build/debug/cai_cli_softline_resize_repro \
  120 30 60 18 retained partial late
```

On the current checkout, the driver returns **0**. With the original 0.10.0
fixture, it returned **1**, reporting `Old source text contaminated the edit
heading`. Failure artifacts are retained in the printed `build/debug/cli-check.*`
directory: `before-resize`, `after-resize-before-write`, `screen`, and
`transcript`. The latter includes terminal scrollback. The driver owns a private
tmux socket and cleans its server on exit.

The C source is [softline_resize_repro.c](../tests/softline_resize_repro.c);
the driver is [softline_resize_repro.sh](../tests/softline_resize_repro.sh).
Its only shared Bash helper is [cli_test_common.sh](../tests/cli_test_common.sh),
which supplies isolated directories, polling assertions and failure cleanup.
No HTTP fixture is started in this reproducer. To build the C program outside
CAI using an installed, matching softline SDK:

```sh
cc -std=c89 -D_POSIX_C_SOURCE=200809L \
  tests/softline_resize_repro.c \
  $(pkg-config --cflags --libs softline) -o softline-resize-repro
```

The normal SDK compiler/runtime requirements still apply to that independent
build. No adjacent repository was modified to obtain these results.

## Controlled sequence

1. Start a real terminal at 120 columns by 30 rows with existing scrollback.
2. Open a softline live output stream with a busy spinner, a welcome status
   message, and a model/context/quota status element.
3. Submit `hold` through `sl_next_prompt`; it returns, retaining the editor frame.
4. On the same owner thread, emit 120 newline-terminated ASCII source rows,
   followed by `RESIZE READY ` and 500 ASCII `x` characters without a final LF.
5. Pause that producer on a control FIFO. The driver captures the real cells,
   resizes to 60 columns by 18 rows, then captures the resized cells before
   releasing the next write. This is a settled resize between writes.
6. On the fixed checkout, explicitly reconcile on the owner before the next
   feed. Continue with LF, `* Edited snake.c`, six numbered patch rows, and a
   final answer. Stop the spinner, set a worked message, and enter the next prompt.
7. Type `draft survives resize` and inspect the actual cells.

All checked softline output/status calls succeeded. The original failure was a
displayed cell assertion, not an API error, missed provider event or parser error.

## Expected versus actual

The heading must occupy a fresh row with exactly:

```text
* Edited snake.c
```

The original 0.10.0 captured cells contained:

```text
xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
* Edited snake.cize reproduction
   1 +PATCH_ROW_000
   2 +PATCH_ROW_001
   3 +PATCH_ROW_002
   4 +PATCH_ROW_003
   5 +PATCH_ROW_004
   6 +PATCH_ROW_005
FINAL ANSWER


! Worked for 0m 1s
+
  gpt-6-luna medium : ctx 5% : ~/workspace : w 99% : crd 588
  16
> draft survives resize
```

`ize reproduction` is the suffix of the old welcome status message. It was
never part of the producer's heading. The final prompt position is correct in
this reproduction; the independently reported prompt-in-middle symptom has
not been reproduced here.

## Boundary and coverage gap

Softline's `sl_native_reconcile_rendered_frame` uses an approximate
bottom-relative/frame/current-line reflow calculation when no input-owned
cursor observation is available. Its documented limitation includes hard-ended
ASCII history, not only Unicode or physical resize racing an actual write.
The shrink also changes the status element from one row to multiple rows.

The published header for `sl_output_stream_write` and softline's
`docs/softline-mdf-stream-design.md` explicitly describe this fallback.
Inspection of that implementation is a localization lead; the report does not
claim that one arithmetic expression alone has been proven to cause it.

Existing passing checks kept readline active at resize, resized while the
provider was paused, or examined output bytes rather than every displayed cell.
The retained frame, busy/status layout, unfinished line and simultaneous
width/height shrink distinguish this failing case. Removing those distinctions
can make a simpler resize test pass.

CAI processes command results and `/resume` replay synchronously after
`sl_next_prompt` returns. The 0.10.0 stream API did not provide an input-owned
observation at this boundary; 0.11.0 adds explicit reconciliation outside feeds.

## Positive controls and repair criteria

The same diagnostic supports `watch`, `yield`, and `retained` owner contexts,
complete/partial final lines and early/late draft entry. Positive cases verify
each edit/patch row and that the unique final prompt is the last nonblank row.
The real CAI cell tests cover a settled provider pause and a resize between
complete writes inside the actual runtime/renderer callback.

All eight cases pass with softline 0.11.0 and owner reconciliation, including the
formerly disabled retained/shrink case. Assertions still require the exact edit
heading, all patch rows, preserved draft and a unique prompt on the last nonblank
row. The CAI unit regression separately proves that a reconciliation error blocks
both ordinary output and quoted prompts, and that a later attempt can retry.

Feed writes remain immediate. Only changed geometry triggers the explicit owner
operation, whose required cursor reply can wait up to 100 ms. No simulated delay,
hardcoded clearing or transcript buffering/replay was introduced. The original
XFCE incident remains untraced; upstream's documented physical-write races and
Unicode/tab reflow limits still apply.
