# lonejson CR: value visitor fails on spaced object string values

## Context

While running cai's OpenRouter tool-calling integration against liblockdc
0.5.0/lonejson 0.5.x, cai received valid tool arguments with ordinary JSON
whitespace:

```json
{"city": "Gothenburg", "code": "openrouter-tool-code-913"}
```

cai's structural tool-argument validator used `lonejson_visit_value_cstr` with
object/key callbacks to reject unknown fields before invoking the tool handler.
The visitor returned `LONEJSON_STATUS_PARSE_ERROR` with:

```text
expected object key
```

The same shape without spaces after separators passed:

```json
{"city":"Gothenburg","code":"openrouter-tool-code-913"}
```

## Minimal Reproduction

Using the value visitor API with object key callbacks:

```c
lonejson_value_visitor v = lonejson_default_value_visitor();
lonejson_value_limits lim = lonejson_default_value_limits();
lonejson_error err;

lonejson_error_init(&err);
v.object_begin = on_event;
v.object_end = on_event;
v.object_key_begin = on_event;
v.object_key_chunk = on_chunk;
v.object_key_end = on_event;

/* Fails with "expected object key". */
lonejson_visit_value_cstr("{\"city\": \"Gothenburg\", \"code\": \"x\"}",
                          &v, NULL, &lim, &err);

/* Succeeds. */
lonejson_visit_value_cstr("{\"city\":\"Gothenburg\",\"code\":\"x\"}",
                          &v, NULL, &lim, &err);
```

Adding string begin/chunk/end callbacks did not change the failure.

## Expected

`lonejson_visit_value_cstr` should accept JSON whitespace anywhere valid JSON
allows whitespace, independent of whether callbacks are installed.

## Current cai Workaround

cai now validates and compacts the arguments first through
`lonejson_json_value_set_buffer` and `lonejson_json_value_write_to_sink`, then
runs the structural visitor over the compacted value. This keeps the validation
path based on lonejson rather than adding ad hoc JSON parsing in cai.
