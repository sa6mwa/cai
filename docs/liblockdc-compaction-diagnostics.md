# liblockdc 0.21.0 compaction diagnostics

Storage compaction and subsequent index recovery succeeded in the reported
incident. The diagnostics make normal deferred cleanup appear to be a failure.
This report concerns the Pouch session store, not model-context compaction.

## Observed sequence

The session log recorded these events on 2026-10-25, in local time:

| Time | Event | Evidence |
| --- | --- | --- |
| 13:08:57 | `compaction.start` | Three candidates; 85,818,695 bytes reclaimable. |
| 13:09:02 | `compaction.complete` | New snapshot segment 6; three cleanup items pending. |
| 13:09:02 | `maintenance.complete` | `compacted:true`, `skipped:false`, `reason:compacted`. |
| 13:09:02 | Two cleanup warnings | Both describe the same three pending items. |
| 13:11:39 | Index-cache warning on reopen | Followed in the same second by successful `index.flush`, `full_rebuild:true`, 13 records. |

The namespace manifest identifies the three deferred objects as two obsolete
segments and one obsolete snapshot, marked obsolete at 13:09:02. These are old
storage artifacts awaiting deletion, not three failed conversations.

## Why cleanup is pending

In the released v0.21.0 sources, `src/lc_pouch_format.h` defaults the deletion
grace period to 15 minutes and the background compaction interval to 30 minutes.
`lc_pouch_namespace_manifest_prune_obsolete_list()` retains artifacts younger
than the grace period. A successful background maintenance pass requeues its
namespace when cleanup is pending.

The grace period is a minimum retention age, not a deletion deadline. Actual
space reclamation awaits a later maintenance pass. Merely opening and listing
the root does not perform obsolete-file cleanup. Restarting also must not be
treated as proof that a previously scheduled maintenance pass ran.

An isolated copy of the reported encrypted store reopened and listed sessions
after the grace period without warnings or errors. Its three obsolete entries
remained, consistent with a read-only open rather than a maintenance pass.
The original store was not modified during this investigation.

## Why the index warning appeared

CAI deliberately opens Pouch with shared-writer operation and indexing enabled.
Before compacting a shared root, `lc_pouch_state_compact_namespace()` retires
the derived query-index manifest so it cannot silently retain a stale view.
`lc_pouch_query_index_invalidate_namespace()` writes the intentional `invalid`
marker. The next cache warm reports that manifest as unreadable; CAI's existing
public index-flush step then rebuilds it from authoritative records before
replay. The reported log confirms that rebuild succeeded.

## Recommended upstream diagnostic changes

- In `src/lc_pouch_state.c`, log routine `compaction.cleanup.pending` and
  `maintenance.cleanup.pending` at debug/info rather than warning. Prefer one
  completion event carrying the pending count over duplicate warnings.
- Rename the `compaction.complete` field `records`: its value comes from
  `cleanup_deleted_count`, so it counts deleted artifacts, not live records.
- Distinguish recognized intentional index invalidation from unexpected cache
  corruption in startup diagnostics. Keep warnings for actual repair failures
  and unexpected corruption; do not suppress all index errors.

The normal pending-cleanup branches return `LC_OK`. Actual obsolete-file deletion
failures return an error with its filesystem cause. Those failures should retain
their warning/error reporting.

## CAI verification

The existing `cai_cli_pouch_compaction` and `cai_cli_pouch_recovery` checks passed
together in 7.94 seconds. They verify encrypted background snapshots, reopened
large payloads and credentials, unchanged JSONL/Markdown exports and encryption
keys, resumed conversation context, and recovery of missing, empty or malformed
derived indexes. No dependency-only test cases were added.

```sh
ctest --test-dir build/debug --no-tests=error --output-on-failure \
  --stop-on-failure --parallel 1 \
  -R '^cai_cli_pouch_(compaction|recovery)$'
```
