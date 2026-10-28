# LQL collection selector stack overflow on musl

Observed on 2026-10-28 with liblockdc 0.23.0 and liblql 0.3.0 in CAI's x86_64
static musl build. The matching native glibc tests passed.

CAI was restoring an ordered collection through one bounded push-to-pull
adapter thread created with default pthread attributes. The collection read
used `eq{field=/record_type,value="commit"}`. On musl, that thread crashed in
the bundled LQL scanner before returning its data:

```text
lql_json_scan_flat_eq_ndjson
lql_stream_apply_flat_eq
lql_stream_apply
lc_collection_read_method
commit_download_write
stream_produce
start
__clone
```

Disassembly of `lql_json_scan_flat_eq_ndjson` shows
`sub $0x2ba78,%rsp`: 178,808 bytes for that frame alone, larger than musl's
default approximately 128 KiB pthread stack. GDB stopped inside that function;
this is not a missing document, invalid encryption key or JSON size-limit error.

An independent disposable program using only public liblockdc APIs reproduced
the defect with a single `{"record_type":"commit"}` object: default pthread
attributes exited with SIGSEGV (139), while the same selector with an explicit
2 MiB thread stack completed successfully. No CAI code was linked into that
probe. Its source and captured results were kept under `build/debug/` during
the investigation; it is not part of the regular regression suite.

For an upstream reproduction, create a Pouch collection containing a commit
object and call its public `read` method with the selector above from a pthread
created with default attributes in a static musl executable. Use a streaming
sink. The failure does not require a large stored document: the scanner's frame
is fixed-size. The same operation on a normal glibc pthread has a larger stack.

CAI's journal restoration now reads the collection directly, without LQL. Its
parser already restores commits and incrementally discards observation data,
so the selector was an unnecessary second parse. The regular journal
round-trip fixture reproduces the affected restart and passes with the rebuilt
static binary, including a response larger than the mapped-string ceiling.

## Resolution verified with liblql 0.4.0

liblql 0.4.0 moves the large workspaces to allocator-backed storage and retains
shared-library ABI 0. The initial verification used liblockdc 0.23.0. CAI now
pins liblockdc 0.24.0 and liblql 0.4.0 for all six Linux SDK targets; both
dependencies keep their bulk parser/query scratch off worker stacks.

On 2026-10-28, a standalone static x86_64 musl program using those published
SDKs created an encrypted Pouch collection containing a commit and an
observation. Its default-attribute pthread read the collection using the
original selector and verified exactly one completed matching record and
nonempty streamed output. It passed without a thread-stack override. This
directly exercises the dependency path that failed with liblql 0.3.0.

The native Debug journal, small-stack review, deletion and Lua application
checks also passed with the upgraded dependency. CAI retains its direct
ordered journal reads and omission of unnecessary workspace indexes for
internal review sessions.
