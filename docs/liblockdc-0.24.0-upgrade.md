# liblockdc 0.24.0 integration

CAI consumes verified release SDKs for its six Linux targets. The upgrade
changes liblockdc's private dependency ABI from 6 to 7; rebuild application
consumers against the matching header and library. CAI's public C layouts are
unchanged. PostgreSQL and cpktdb remain outside the selected dependency closure.

## Append and retention paths

Small journal and prompt records already use pull append with lonejson's
incremental generator on the calling thread. Keep that path: it shares active
segments and durable group commits. The new push writer is useful for a native
push producer, but costs a segment per incremental commit. Switching these
existing pull producers would add work without removing a thread or body copy.

Journal append requests now carry the whole-collection retention directive.
The SDK publishes the entry and resolved parent deadline atomically. CAI no
longer issues a second collection-metadata mutation after every event or
context edit. Session metadata keeps its existing retention renewal; identical
append retries renew neither deadline. Transport acknowledgement recovery still
checks append identity/status before acknowledging an uncertain write.

Prompt recall configures the collection's native `max_entries` policy from the
existing limit, default 200. The existing SDK worker performs lazy oldest-first
pruning; foreground append never scans or deletes prompt history. Unchanged
policies skip metadata writes. Zero clears the count cap and disables recall
and recording, preserving saved prompts and avoiding collection creation when
none exists. Failed policy changes retain the active recall receiver.
Conversation journals have no count cap, and whole-conversation inactivity
retention remains 30 days by default.

## Compression changes

CAI explicitly selects zlib by default, or none with `disable_compression`.
liblockdc now converts an existing root synchronously when that explicit mode
changes. Conversion preserves data, collection positions, retry identities,
and retention; it needs space for a replacement tree and exclusive writer
ownership. A competing writer prevents conversion and produces a warning with
adoption of the stored mode. Conversion errors fail open rather than report
success. Matching-mode opens do not perform conversion scans.

The application still shares one client per equal Pouch root for the lifetime
of its contexts. Conflicting settings for an already shared process client are
rejected. There is no per-operation open/close or compression middleware.

## Disposable startup measurement

On 2026-10-29, the same public-SDK-only probe ran serially against published
0.23.0 and 0.24.0 SDKs on this host. Both used cpkt 0.6.1, liblql 0.4.0,
encrypted fresh roots, zlib and durable sync. Each case performed 128 operations
and closed cleanly before reopening. No user store was opened or altered.

| Case | 0.23.0 write/close | First reopen | 0.24.0 write/close | First reopen |
| --- | ---: | ---: | ---: | ---: |
| Lease retention renewals | 4.892 s | 0.953 s | 6.243 s | 0.003 s |
| Acquire/release without renewal | 1.690 s | 0.010 s | 1.844 s | 0.001 s |
| Collection metadata renewals | 1.305 s | 0.023 s | 0.840 s | 0.005 s |

The old lease-renewal case required 512 recovery reads, 128 deletes and 128
fsyncs at first reopen. The new case required none. Its second reopen was also
3 ms. This confirms the completed-transaction startup-backlog fix (BUG-052).
Cleanup moves to successful release, so the startup improvement is not a claim
that lease writes became faster. These are single disposable measurements,
not stable throughput thresholds or a permanent timing test.

Integration checks use real encrypted collections for retention directives,
duplicate retries, count-policy persistence and failure handling. Offline CLI
checks verify both compression directions with identical retained context and
observations, ignoring only the portable export's generated timestamp. C/Lua
embedding, messaging, kanban, recovery and deletion exercise the upgraded SDK.
The rebuilt x86_64 static musl CAI also passed the isolated review/replay
scenario using its default worker-thread stacks and real encrypted storage.
