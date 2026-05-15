# lonejson CR: Selected Array Streaming Rewriter

## Status

New CR written after cai upgraded to lonejson 0.15.0.

This replaces the older `stash/lonejson-array-item-streaming-cr.md` direction
where the proposed fix was to replace the read cursor entirely. In 0.15.0 the
read side has become useful and should stay:

- `lonejson_array_stream_*` streams selected root/direct arrays.
- `LONEJSON_FIELD_MAPPED_ARRAY_STREAM[_REQ]` streams mapped object arrays
  during normal mapped parsing.
- Nested mapped array streams compose through normal maps.

The remaining gap is mutation: cai needs to rewrite selected arrays while
preserving true streaming semantics.

## Problem

cai's persisted todo/kanban tool currently uses an object-framed JSON record
stream:

```json
{"type":"board","id":"...","name":"main"}
{"type":"item","id":"...","board_id":"...","title":"..."}
```

That format is correct for constrained memory because cai can read and rewrite
one record at a time. It is less ideal for humans and external tools than a
single JSON document such as:

```json
{
  "version": 1,
  "boards": [
    {
      "id": "b1",
      "name": "main",
      "wip_limit": 1,
      "items": [
        { "id": "i1", "title": "ship cai", "status": "todo" }
      ]
    }
  ],
  "done": []
}
```

lonejson 0.15.0 can stream-read this shape, including nested mapped item
arrays. It still cannot rewrite `boards`, `items`, or `done` item-by-item
without the caller materializing the enclosing document or selected array.

For cai, materializing the todo store is not acceptable. Hidden temporary
full-document buffers or "serialize once then write" are also not acceptable;
that is faux streaming.

## Required Feature

Add a selected-array streaming rewriter/transformer API.

Core behavior:

- Input from `lonejson_reader_fn`, `FILE *`, fd, or path.
- Output to `lonejson_sink_fn`, `FILE *`, fd, or path.
- The input JSON document is parsed and validated.
- The selected array is delivered item by item.
- The output document is emitted incrementally.
- Unselected document parts are copied through or canonically reserialized.
- The whole document and whole selected array are never materialized.
- At most the current selected item may be materialized/spooled.
- Normal lonejson parse options and limits apply.
- Reader, sink, parse, duplicate-key, type, depth, overflow, callback, and
  invalid-action errors surface through `lonejson_error`.

## Selector Scope

Minimum v1:

- `""` selects a root array.
- `"items"` selects a direct array field on the root object.
- `"boards"` selects a direct array field on the root object.
- Dotted paths such as `"boards.items"` are rejected.
- No implicit fan-out through arrays in v1.

Useful later extension:

- Explicit path syntax for nested arrays, for example `boards[].items`.
- If added, the API must make parent context delivery explicit and bounded.

## Item Delivery

The callback should support mapped item delivery:

```c
typedef lonejson_status (*lonejson_array_rewrite_item_fn)(
    void *user,
    void *item,
    lonejson_error *error);
```

The rewriter should also support arbitrary JSON item delivery via
`lonejson_json_value` for cases where a caller wants to preserve unknown item
fields.

Recommended item delivery variants:

- mapped item via `const lonejson_map *item_map` and reusable `void *item_dst`,
- `lonejson_json_value` capture or stream/visitor mode,
- optional later raw per-item reader only if there is a real zero-spool
  pass-through use case.

## Rewrite Actions

The item callback must be able to choose the output action for each item.

Minimum actions:

```c
typedef enum lonejson_array_rewrite_action {
  LONEJSON_ARRAY_REWRITE_KEEP,
  LONEJSON_ARRAY_REWRITE_DROP,
  LONEJSON_ARRAY_REWRITE_REPLACE
} lonejson_array_rewrite_action;
```

Recommended full action set:

```c
typedef enum lonejson_array_rewrite_action {
  LONEJSON_ARRAY_REWRITE_KEEP,
  LONEJSON_ARRAY_REWRITE_DROP,
  LONEJSON_ARRAY_REWRITE_REPLACE,
  LONEJSON_ARRAY_REWRITE_INSERT_BEFORE,
  LONEJSON_ARRAY_REWRITE_INSERT_AFTER,
  LONEJSON_ARRAY_REWRITE_REPLACE_AND_INSERT_AFTER
} lonejson_array_rewrite_action;
```

Append support is required. It can be represented as:

- an end-of-array callback that emits zero or more appended items, or
- an explicit append list/source configured before rewriting.

The rewriter must own array framing and comma placement. Callers should never
write `[` / `]` / `,` glue themselves.

## Replacement / Insert Sources

Replacement and inserted items should support:

- mapped struct plus `lonejson_map`,
- `lonejson_json_value`,
- optional later `lonejson_source`/reader for one already-valid JSON item if
  the validation and replay semantics are clear.

For cai todo, mapped structs are enough for `board`/`item` rewrites and append.
`lonejson_json_value` is useful if we later preserve host-owned unknown
metadata.

## File Helper

The core API can stay sink/source based. A path helper would be useful:

```c
lonejson_status lonejson_array_rewrite_path(
    const char *selector,
    const char *input_path,
    const char *output_path,
    const lonejson_array_rewrite_options *options,
    lonejson_error *error);
```

Atomic temp-file/rename can remain caller responsibility. cai already needs a
lockfile and transaction boundary, so it can write to a temp path and rename on
success.

## cai Todo Use Cases

With this API cai can move from object-framed records to a single JSON document
without losing memory bounds:

- `create_board`: append one board to `boards`.
- `set_wip_limit`: replace one board in `boards`.
- `add_item`: append one item to a selected board's `items` once nested path
  rewriting exists; until then a flat root `items` array can be used.
- `move_item`: replace one item status.
- `complete_item`: drop from active `items`, append to `done`.
- `list_board`: can continue using 0.15.0 nested mapped-array read streaming.

If v1 only supports direct root arrays, cai can use this normalized shape:

```json
{
  "version": 1,
  "boards": [],
  "items": [],
  "done": []
}
```

That avoids nested rewrite paths while still giving users a single JSON
document.

## Non-Goals

- No DOM.
- No full-document materialization.
- No full-array materialization.
- No hidden spool of the complete input/output document.
- No caller-managed comma/framing glue.
- No implicit dotted-path fan-out in v1.

## Acceptance Criteria

- Rewrites a root array item-by-item with keep/drop/replace/append.
- Rewrites a direct root object array field item-by-item.
- Preserves valid JSON output for empty arrays, first/last item changes, and
  all-drop/all-append cases.
- Validates unselected document parts.
- Rejects malformed JSON and duplicate keys under default parse options.
- Fails cleanly on callback and sink errors.
- Demonstrates bounded memory with large arrays.
- Includes tests where replacement items contain large spooled/string fields.
- Includes tests for `lonejson_json_value` item preservation.

## Why This Matters

lonejson 0.15.0 solves the read side well enough for cai's nested todo queries.
The missing write-side primitive is now the only reason cai keeps the todo
store as object-framed records instead of a normal single JSON document.

