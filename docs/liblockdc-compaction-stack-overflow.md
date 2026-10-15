# liblockdc: background Pouch compaction overflows musl thread stack

## Status and impact

Root cause identified from an actual CAI core dump on 2026-10-15. Repair belongs
in liblockdc; this report does not change that repository or patch its packaged
archive. CAI's current static musl build remains affected.

Background Pouch compaction can terminate the entire application with SIGSEGV
while its interactive UI is idle. The failure happens while writing a snapshot,
before the payload-copy function opens its input file. Data integrity after this
specific interruption has not been established. A subsequent unreadable derived
query-index warning was reported, but its causal relationship remains unproven.

## Artifacts and matching

- Core: `~/core._home_mike__local_bin_cai.1000.cebc8433-f180-4a77-8bd5-a1e470e7f0cd.735738.204958272`.
- Matching executable preserved by the user: `~/cai`, static Linux x86_64 musl,
  with debug information and an unstripped symbol table.
- Executable SHA-256:
  `456809159bdc061853da6543c65099541ffb7f840f48d8c586daa6915746aa8f`.
- The core records `/home/mike/.local/bin/cai` as the original executable path.
  Its ELF entry point and load-segment addresses/sizes match `~/cai`.
- CAI currently links the packaged liblockdc 0.19.0 static archive. The same
  failing function and stack allocation are present in `build/cai/cai`.

The installed binary and current build have been replaced since the crash.
Using their symbols with this core produces incorrect function names. GDB must
also read executable code from the matching file, rather than the now-replaced
path recorded in the core's file mappings; use `set trust-readonly-sections on`.

## Backtrace from the matching executable

Faulting LWP: 735740. Signal: SIGSEGV, `si_code = 2` (`SEGV_ACCERR`).

```text
#0  0x634f41 lc_pouch_state_copy_file_span_to_fd_crc
#1  0x64bf0b lc_pouch_state_write_snapshot
#2  0x6592b7 lc_pouch_maintenance_run_locked
#3  0x65a59f lc_pouch_maintenance_run
#4  0x5eb16c lc_pouch_compaction_run_pass
#5  0x5eb7c5 lc_pouch_compaction_worker
#6  0xc7b73f start
#7  0xc7cfee __clone
```

The main thread was blocked in `poll`; the other four threads were waiting in
`__timedwait_cp`. This explains a crash with no active agent turn.

## Instruction and memory evidence

The matching executable contains:

```asm
0x634f30: push %r15
0x634f32: push %r14
0x634f34: push %r13
0x634f36: push %r12
0x634f38: push %rbp
0x634f39: push %rbx
0x634f3a: sub  $0x20068, %rsp
0x634f41: mov  %rdi, 0x28(%rsp)  # fault
```

At the fault:

```text
RSP          = 0x7cccd0a57c00
si_addr      = 0x7cccd0a57c28 = RSP + 0x28
guard range  = [0x7cccd0a56000, 0x7cccd0a58000), no access
```

The function subtracts 131,176 bytes from the stack, in addition to its saved
registers and callers' frames. Its first write lands inside the guard mapping.
The saved return address at `0x7cccd0a77c98` is `0x64bf0b`, independently matching
the snapshot caller after undoing that frame allocation and six register pushes.

Read-only inspection of the adjacent liblockdc checkout identifies the source:

```c
/* src/lc_pouch_state.c: lc_pouch_state_copy_file_span_to_fd_crc */
unsigned char buffer[128U * 1024U];
```

`lc_pouch_compaction_worker_init` creates the worker with
`pthread_create(..., NULL, ...)`, using the platform's default thread stack.
This stack allocation exceeds the available space on this musl worker once
caller frames are included. It does not depend on the size of the copied span:
the fault occurs on function entry.

## Recheck the supplied evidence

Run from this repository, keeping both original artifacts available:

```sh
gdb -nx -batch \
  -iex 'set auto-load off' \
  -iex 'set debuginfod enabled off' \
  -ex 'set pagination off' \
  -ex 'set trust-readonly-sections on' \
  -ex 'thread apply all bt 18' \
  -ex 'thread 1' \
  -ex 'p $_siginfo' \
  -ex 'info registers rip rsp' \
  -ex 'x/8i $pc-17' \
  "$HOME/cai" \
  "$HOME/core._home_mike__local_bin_cai.1000.cebc8433-f180-4a77-8bd5-a1e470e7f0cd.735738.204958272"
```

This diagnoses the captured failure; a fresh isolated compaction reproducer has
not been run as part of this investigation. Ten-second idle/index-publication
checks previously run in CAI did not force a snapshot containing payload spans,
so their success did not cover this failure.

## Recommended upstream repair and proof

Move the bounded copy scratch buffer off the worker stack, using the Pouch
allocator with checked allocation and cleanup on every exit, or use a suitably
small bounded stack chunk. Preserve direct chunked copying, incremental CRC,
throttling, error propagation and constant memory use. Increasing every caller's
thread stack would leave the oversized local allocation in library code.

Required regression coverage:

1. On static musl, create an isolated Pouch with a live payload span and force
   background compaction to write a snapshot on its actual default-stack worker.
   Require completion and successful reopen/readback; an idle wait alone is
   insufficient.
2. Exercise small and multi-chunk payloads, encryption/compression settings,
   repeated snapshots, and multiple namespaces. Check bytes and CRC/readback.
3. If allocation is introduced, inject allocation failure and confirm a reported
   maintenance error, retryability and intact authoritative records. Cover read,
   write and truncated-input failures and cleanup of partial snapshots.
4. Run the copy/snapshot path on an explicitly small pthread stack as a focused
   guard against recurrence, alongside the real musl worker test.
5. Terminate an isolated writer during snapshot publication and verify supported
   recovery, including authoritative payloads and derived-index rebuilding.
6. After consuming the upstream fix, run a CAI static-musl regression that forces
   the same maintenance path with encrypted session data and verifies resume and
   export after reopening.
