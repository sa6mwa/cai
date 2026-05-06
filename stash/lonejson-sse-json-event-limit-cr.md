# lonejson CR: enforce max_event_data_bytes in SSE JSON mode

## Context

cai moved its Responses streaming parser from a local SSE `data:` accumulator to
`lonejson_sse_push_json` so SSE event data streams directly into lonejson's JSON
parser.

`lonejson_sse_push` documents and enforces `max_event_data_bytes` while
streaming `data:` chunks. `lonejson_sse_push_json` exposes the same
`lonejson_sse_options`, but the current implementation appears to increment
`json_data_len` without checking `options.max_event_data_bytes`.

## Expected

`lonejson_sse_push_json` should enforce `max_event_data_bytes` before feeding
each `data:` chunk into the JSON parser, including the inserted newline between
multiple `data:` lines.

This should fail with `LONEJSON_STATUS_OVERFLOW` before unbounded mapped-field
allocation can occur.

## Why cai cares

cai streams OpenAI Responses SSE directly into `lonejson_sse_push_json`. It
does not want to add a hidden per-event staging buffer just to enforce a size
cap, because that reintroduces faux-streaming behavior at the SSE layer.

## Suggested Acceptance Tests

- One `data:` line larger than `max_event_data_bytes` fails with
  `LONEJSON_STATUS_OVERFLOW`.
- Multiple `data:` lines whose combined payload plus inserted newlines exceeds
  `max_event_data_bytes` fail with `LONEJSON_STATUS_OVERFLOW`.
- The failure occurs before completing parse into mapped destination fields.
- Plain `lonejson_sse_push` and `lonejson_sse_push_json` enforce equivalent
  event-data limits.
