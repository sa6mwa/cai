# lonejson CR: public spooled append API

## Problem

cai needs to append streamed chunks into a `lonejson_spooled` value while
preserving lonejson's memory-limit and spill-to-disk behavior. Current public
APIs allow callers to read, rewind, reset, cleanup, and write a spooled value to
a sink, but they do not expose an append sink for caller-owned streamed bytes.

The result is awkward in downstream SDK code:

- streamed session history has no direct public append target,
- tool output capture cannot naturally produce a spooled JSON string payload,
- generated request JSON cannot be kept in the same spooled abstraction without
  either materializing or adding downstream buffering,
- downstream code is tempted to touch `lonejson_spooled` internals, which should
  remain off limits.

## Requested API

Add a public append helper:

```c
lonejson_status lonejson_spooled_append(lonejson_spooled *value,
                                        const void *data,
                                        size_t len,
                                        lonejson_error *error);
```

Expected behavior:

- `value` must be initialized by `lonejson_spooled_init` or
  `lonejson_spooled_init_with_allocator`.
- `data == NULL && len > 0` is invalid.
- `len == 0` is a no-op.
- Appended bytes preserve order and increase `lonejson_spooled_size(value)`.
- Existing `memory_limit`, `max_bytes`, allocator, and `temp_dir` settings are
  honored.
- When appending crosses `memory_limit`, data spills to the existing lonejson
  temporary-file machinery.
- If `max_bytes` would be exceeded, return the same status/error style used by
  streamed parse/spool overflow today.
- The read cursor should remain predictable. Recommendation: appending should
  not implicitly rewind; callers that want to read from the beginning should use
  `lonejson_spooled_rewind`.

## cai temporary workaround

cai currently uses a compatibility helper that rebuilds spooled values through
lonejson's public streaming parser. It avoids private-field access, but it is
not the desired long-term implementation because append-heavy paths become
unnecessarily expensive.

Once this API is available in an official lonejson release, cai should replace
the compatibility helper with a direct call to `lonejson_spooled_append`.
