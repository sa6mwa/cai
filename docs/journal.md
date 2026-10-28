# Session journals

`libcai` persists ordered changes. `libcai_app` stores them in one liblockdc
collection per conversation, alongside the runtime's replayable observation
events. Recovery reads the collection in order; it neither loads recurring
full-state snapshots nor executes recorded tools.

Console replay uses the same native tool renderer as live output. The recovery
pass indexes bounded command/path labels and completion metadata, not result
bodies, and evicts calls outside the selected replay tail. The rendering pass
decodes command output directly from ordered execution chunks into the display
sink, including JSON escapes split between records. There is no additional
collection scan, spool or per-tool thread. Completed command output is grouped
across `exec_command`/`write_stdin` calls by source session, terminal and command
identity; compact output keeps the normal preview limit for that whole command.
Known small structured results use the live application presenter. Other result
bodies flow through the bounded tool preview instead of disappearing.

Successful patches additionally record `tool.patch_locations` and `tool.patch`
execution fields from the confirmed live preview. These preserve context and
line numbers without rereading workspace files during replay. Submitted patch
arguments remain available separately for inspection. Invalid saved command
JSON is reported as a replay diagnostic, leaving the original JSONL available.

A durable `commit` record contains an incremental state edit and the watermark
of input/control events already reflected in that edit. Normal edits append only
the new canonical context bytes. Trim records retain a prefix without copying
it again. Model-context compaction explicitly replaces context with the new
summary and retained provider items. Turn boundaries, hidden event ranges and
event ownership are also prefix/append edits. Small commit metadata preserves
settings, continuation handles, measured context usage and goal state.
Cumulative token counts and USD/EUR estimates are restored independently of
the most recent response's usage.

Revisions must increase consecutively. Restoration checks prefixes, lengths,
metadata and complete record boundaries. The payload's headers precede its
`history_append` string so the parser can decode it directly into retained
context. Sources and parsers use bounded chunks; there is no whole-journal
download, full-state reconstruction file or producer thread per entry.

The collection journal is authoritative. Session/directory documents are small
indexes for selection and listing. If an index publication fails after a durable
journal append, the append remains committed; dirty-index recovery streams commit
headers to repair selection. A stale index watermark cannot suppress newer
committed state. Pending events after the recovered watermark remain eligible
for the existing input/control replay path.

The dependency-free `cai_agent_session_store` contract follows the same model.
Its `commit` callback atomically appends an edit, and its load callbacks return
the entire ordered journal. The built-in core backend uses append-only JSONL.
`cai_session.restore_journal_source` and Lua `session:restore_journal(reader)`
perform restoration without running tools. Explicit complete-state import/export
APIs remain available for callers requesting snapshots; they are separate from
runtime persistence and use spools.

Pouch compression defaults to streaming zlib before encryption. Disable it with
`--disable-compression`, `CAI_DISABLE_COMPRESSION=true`, or:

```ini
[storage]
disable_compression = true
```

Embedders can set the same schema field through application settings. Standalone
kanban receivers expose `disable_compression` in their C/Lua configuration too.
Equal Pouch roots still share one process client; conflicting compression modes
are rejected. Remote lockd clients do not receive local compression settings.
The configured mode must match the root manifest. This cutover does not migrate
old snapshot-backed stores; use a new root.

Portable JSONL export excludes trimmed entries. It streams the visible
observation events and then one initialization commit for the retained
context. This deliberate export-only initialization is separate from live
incremental persistence. Import assigns a new conversation ID in the selected
workspace, validates the journal, and publishes it only after successful
restoration. Imported conversations are ordered by import time for selection;
their original event/commit timestamps remain available in the records.
Markdown continues to use the existing libcai conversation/execution exporters.

`cai_agent_runtime_export_journal_state(runtime, sink, error)` (Lua
`runtime:export_journal_state(writer)`) streams that initialization commit at a
stable owner-thread boundary. It neither commits live state nor advances its
persistence cursor. Restore into a fresh session; discard that session if
restoration fails, because earlier valid edits may already have been applied.

If a Pouch append loses its acknowledgement, the adapter asks liblockdc for
that append ID's status before acknowledging it. An unresolved status prevents
further journal writes through the shared client until reopening. The local
JSONL store similarly rejects further writes after a sync failure on a complete
record; reopening recovers complete records and ignores an incomplete tail.
