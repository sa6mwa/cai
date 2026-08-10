# CR: primitive JSON value writer APIs for lonejson

## Problem

cai must not implement JSON escaping, token classification, or wrapper trimming
outside lonejson. Most cai JSON transport now goes through mapped lonejson
serializers, `lonejson_json_value`, visitors, selected-array streams, and
rewriters.

One remaining class is awkward with the current public API: serializing one
primitive value, especially a raw byte/string source as a JSON string, directly
to a sink/generator without inventing a dummy object wrapper.

Current cai references that should disappear once this exists:

- `src/cai_response.c`: `cai_json_builder_string()`
- `src/cai_response.c`: `cai_json_builder_string_spooled()`
- `src/cai_agent.c`: `cai_agent_json_builder_spooled_string()`

Today cai can serialize:

```c
typedef struct doc {
  lonejson_spooled value;
} doc;

static const lonejson_field fields[] = {
  LONEJSON_FIELD_STRING_STREAM_REQ(doc, value, "value")
};
```

But that emits `{"value":"..."}`. cai needs only the `"..."` JSON string in
places where it is building a larger request or history fragment. The old
workaround was to serialize the wrapper and trim `{"value":` and the closing
`}`. That keeps escaping in lonejson, but the wrapper trimming is still a
bespoke JSON-shape assumption and should not exist.

The Lua binding has a related issue. cai used to include a Lua-table-to-JSON
encoder in `lua/cai_lua.c`. That has been removed because JSON serialization
must be owned by lonejson. cai can accept raw JSON strings and streamed sources
from Lua, but it cannot expose table-as-raw-JSON without a lonejson-provided
schema-free value serializer.

## Requested API

Add first-class primitive value writer/generator APIs:

```c
lonejson_status lonejson_write_json_string_sink(
    lonejson_reader_fn reader, void *reader_user,
    lonejson_sink_fn sink, void *sink_user,
    const lonejson_write_options *options,
    lonejson_error *error);

lonejson_status lonejson_write_json_string_buffer_sink(
    const void *data, size_t len,
    lonejson_sink_fn sink, void *sink_user,
    const lonejson_write_options *options,
    lonejson_error *error);

lonejson_status lonejson_write_json_string_spooled_sink(
    const lonejson_spooled *value,
    lonejson_sink_fn sink, void *sink_user,
    const lonejson_write_options *options,
    lonejson_error *error);
```

Generator equivalents would be even better for transport paths:

```c
lonejson_status lonejson_string_generator_init(
    lonejson_generator *generator,
    lonejson_reader_fn reader, void *reader_user,
    const lonejson_write_options *options);
```

If a generic value writer is a better fit, expose a `lonejson_json_scalar` or
`lonejson_json_value_builder` that can be configured from string, number,
boolean, null, source, or spooled data and then written through the same
generator/sink interfaces as mapped structs.

## Lua binding request

Expose a lonejson-owned schema-free value encoder for native Lua values:

```lua
local json = lonejson.encode_value(value)
schema:encode_value_to_sink(value, sink)
```

It must use lonejson's serializer/visitor machinery, not a separate Lua JSON
implementation. This lets cai accept Lua tables for metadata/raw schemas/tool
payloads without carrying its own JSON encoder.

## Requirements

- No dummy object wrapper required by callers.
- No caller-side trimming of object prefixes/suffixes.
- String escaping is entirely inside lonejson.
- Source/spooled variants stream through bounded chunks.
- Generator variants must not materialize the whole JSON string.
- Errors should distinguish invalid arguments, source read failures, and sink
  failures.

## Tests

- Empty string, ASCII, quotes, backslashes, control characters, UTF-8.
- Large spooled string that spills to disk.
- Reader-backed string emitted in small chunks.
- Sink failure propagation.
- Generator reads with tiny output buffers.
- Lua `encode_value` for objects, arrays, strings, numbers, booleans, null, and
  nested mixed values.
