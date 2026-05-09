# lonejson CR: Streaming JSON Array Item Reader/Rewriter

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

Current lonejson supports object-framed streams with `lonejson_stream_next()`,
and it supports semantic value visitors with `lonejson_visit_value_*()`. Those
are enough to build a true streaming store if the file is represented as
consecutive JSON object records. They are not enough to conveniently stream
one item at a time from an array inside a single JSON document while also
preserving and re-emitting the surrounding document.

## Requested Feature

Add an API that can stream array elements from a selected path in a JSON
document without materializing the whole array or root document.

Desired capabilities:

- Parse from `FILE *`, fd, path, or `lonejson_reader_fn`.
- Select an array by a simple object-key path such as `boards` or
  `boards.items`.
- Yield each element as either:
  - a mapped struct through a `lonejson_map`, or
  - a `lonejson_json_value`/reader for callers that want to pass through or
    transform the item.
- Enforce normal lonejson parse options and limits.
- Preserve bounded memory: only the current element may be buffered/spooled.
- Surface duplicate-key, malformed JSON, type mismatch, and overflow errors
  with normal `lonejson_error` detail.

## Optional Rewriter

A paired streaming rewriter would be very useful:

- Copy all unchanged JSON from input to output.
- For each selected array element, call a callback with the parsed item.
- Callback returns one of:
  - keep original item,
  - replace item by serializing a mapped struct,
  - drop item,
  - insert one or more items before/after.
- Allow appending at the end of a selected array.
- Keep object/array framing valid without the caller manually managing commas.

## Why This Matters

Without this, cai must use an object-framed JSON record stream for the todo
store to preserve true streaming semantics. That is technically correct and
uses lonejson honestly, but a single-document JSON format is easier for users
and external tools to inspect.

This is not a request for a DOM. It is specifically an item-at-a-time streaming
cursor/rewrite primitive for arrays inside otherwise normal JSON documents.
