# lonejson CR: Replace Array Read Cursor With Selected Array Transformer

Status: this supersedes the read-cursor-only CR. For a single-header library,
avoid keeping a narrow transitional API plus a second rewrite API. Prefer one
coherent selected-array transformer primitive that can serve both read-only
iteration and streaming rewrite.

## Problem

cai's persisted todo/kanban tool needs to mutate large JSON-backed stores under
a constrained memory budget. The natural storage format is a single JSON
document with arrays, for example:

```json
{
  "version": 1,
  "boards": [
    { "id": "abc", "name": "default", "items": [ ... ] }
  ]
}
```

Current lonejson 0.7.0 supports object-framed streams with
`lonejson_stream_next()`, selected-array read cursors with
`lonejson_array_stream_next*()`, and semantic value visitors with
`lonejson_visit_value_*()`. The selected-array read cursor is useful, but it is
too narrow as a long-term public API if the library also needs selected-array
rewrite. cai needs mutation, not only read-side scans.

## Remove

Remove the selected-array read cursor API as a standalone public surface if it
is not intended to stay permanently:

- `lonejson_array_stream_open_*`
- `lonejson_array_stream_next`
- `lonejson_array_stream_next_value`
- `lonejson_array_stream_push*`
- `lonejson_curl_array_parse*`

Keep only the parts that still make sense as internal machinery for the
transformer, or keep the names only if they become compatibility wrappers over
the transformer before any stable release promise exists.

Do not keep both a read cursor family and a transformer family unless both are
explicitly worth supporting long-term.

## Implement

Implement a single selected-array transformer API family.

Core behavior:

- Input from `lonejson_reader_fn`, `FILE *`, fd, or path.
- Optional output to `lonejson_sink_fn`, `FILE *`, fd, or path.
- `output == NULL` means read-only iteration.
- Selector supports root array `""` and one direct root object key such as
  `items` or `boards`.
- No dotted paths and no implicit fan-out in v1.
- The transformer owns JSON object/array framing and comma handling.
- It copies all unselected parts of the source document to output unchanged or
  canonically rewritten, depending on what is realistic for lonejson internals.
- It never materializes the whole document or selected array.
- It may buffer/spool only the current selected item.
- Enforce normal lonejson parse options and limits.
- Surface malformed JSON, duplicate-key, type mismatch, overflow, sink, reader,
  and callback failures through `lonejson_error`.

Item delivery:

- Mapped struct through a `lonejson_map`.
- Captured `lonejson_json_value`.
- Optional later: per-item reader/raw stream if a real zero-spool pass-through
  use case appears.

Callback actions:

```c
typedef enum lonejson_array_transform_action {
  LONEJSON_ARRAY_TRANSFORM_KEEP,
  LONEJSON_ARRAY_TRANSFORM_DROP,
  LONEJSON_ARRAY_TRANSFORM_REPLACE
} lonejson_array_transform_action;
```

Recommended full action set:

```c
typedef enum lonejson_array_transform_action {
  LONEJSON_ARRAY_TRANSFORM_KEEP,
  LONEJSON_ARRAY_TRANSFORM_DROP,
  LONEJSON_ARRAY_TRANSFORM_REPLACE,
  LONEJSON_ARRAY_TRANSFORM_INSERT_BEFORE,
  LONEJSON_ARRAY_TRANSFORM_INSERT_AFTER,
  LONEJSON_ARRAY_TRANSFORM_REPLACE_AND_INSERT_AFTER
} lonejson_array_transform_action;
```

Append support is required for cai's todo `add_item`. It can be represented as
an end-of-array callback or an explicit append API.

Replacement/insert source should support:

- mapped struct + `lonejson_map`,
- `lonejson_json_value`,
- optional later: direct sink callback if needed.

Read-only mode:

- Use the same transformer API with no output sink.
- Callback receives items and returns `KEEP` or a read-only equivalent.
- Mutation actions should be rejected when output is absent.

Path semantics:

- `""` selects the root array.
- `"items"` selects a direct array field on the root object.
- `"boards.items"` is rejected in v1.
- No implicit array fan-out in v1.

Atomic file helper:

The core API should write to caller-provided sinks/paths. A path-to-path helper
may optionally write to a temp file and rename, but that can also remain caller
responsibility. cai can manage lock/temp/rename itself.

## Why This Matters

Without this, cai must keep using an object-framed JSON record stream for the
todo store to preserve true streaming semantics. That is technically correct
and uses lonejson honestly, but a single-document JSON format is easier for
users and external tools to inspect.

This is not a request for a DOM. It is specifically an item-at-a-time streaming
transform primitive for arrays inside otherwise normal JSON documents.
