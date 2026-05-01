# Lonejson CR: Dynamic Object Writer For Generated Schemas

## Context

CAI now routes Responses requests, Conversations item payloads, local history
export, and large tool output paths through typed lonejson maps and spooled
readers. The remaining intentional JSON builder path is tool JSON Schema
generation.

Tool schemas are not fixed structs. They are dynamic JSON objects where
`properties` is keyed by caller-defined parameter names:

```json
{
  "type": "object",
  "properties": {
    "city": { "type": "string" },
    "days": { "type": "integer" }
  },
  "required": ["city"],
  "additionalProperties": false
}
```

Lonejson maps are excellent for known fields, but they cannot currently stream a
JSON object whose member names come from runtime data. CAI therefore validates
raw property fragments with `lonejson_json_value`, then uses a small internal
JSON builder to assemble the schema object.

## Request

Add a public dynamic-object serialization API that can stream runtime property
names and mapped or raw JSON values without materializing the full object.

One possible shape:

```c
typedef struct lonejson_object_writer lonejson_object_writer;

lonejson_status lonejson_object_writer_init(lonejson_object_writer *writer,
                                            lonejson_sink_fn sink,
                                            void *sink_user,
                                            const lonejson_write_options *opts,
                                            lonejson_error *error);

lonejson_status lonejson_object_writer_field_string(
    lonejson_object_writer *writer, const char *key, const char *value,
    lonejson_error *error);

lonejson_status lonejson_object_writer_field_json_value(
    lonejson_object_writer *writer, const char *key,
    const lonejson_json_value *value, lonejson_error *error);

lonejson_status lonejson_object_writer_field_map(
    lonejson_object_writer *writer, const char *key, const lonejson_map *map,
    const void *value, lonejson_error *error);

lonejson_status lonejson_object_writer_begin_array(
    lonejson_object_writer *writer, const char *key, lonejson_error *error);

lonejson_status lonejson_object_writer_array_string(
    lonejson_object_writer *writer, const char *value, lonejson_error *error);

lonejson_status lonejson_object_writer_end_array(lonejson_object_writer *writer,
                                                 lonejson_error *error);

lonejson_status lonejson_object_writer_finish(lonejson_object_writer *writer,
                                              lonejson_error *error);
```

The exact API can differ, but CAI needs these properties:

- Runtime field names are escaped by lonejson.
- Values can be mapped structs, strings, arrays, sources/spooled strings, and
  validated `lonejson_json_value`.
- Output goes to any lonejson sink and to `lonejson_generator`-style pull APIs
  if feasible.
- The writer tracks commas and object/array nesting internally.
- Invalid state transitions fail with actionable errors.
- The API does not require a full in-memory object buffer.

## Why CAI Needs It

Without this, generated JSON Schema objects are the one place where CAI must keep
a bespoke JSON builder. That builder can be correct, but it is not the desired
architecture: lonejson should own JSON escaping, object framing, and streaming
for both fixed and dynamic JSON objects.

## Acceptance Criteria

- CAI can rebuild `cai_tool_schema_rebuild()` without hand-writing object
  punctuation.
- CAI can add or describe a tool property while preserving existing dynamic
  property names.
- Property fragments already validated as `lonejson_json_value` can be streamed
  into the dynamic object without reparsing into a temporary full buffer.
- Tests cover object fields with escaped names, empty objects, nested arrays,
  `json_value` fields, and sink/generator chunk boundaries.
