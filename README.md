# cai

`cai` is a C89/POSIX SDK-style client for the OpenAI Responses API,
Conversations, and Realtime WebSocket workflows.

The goal is a small, handle-oriented C API that fits systems such as Vectis:
stream-first data flow, explicit ownership, predictable error handling, and
good ergonomics for agentic workflows without forcing application code to build
raw HTTP requests or hand-roll JSON.

See [PLAN.md](PLAN.md) for the current implementation plan.

Responses WebSocket support is not treated as a normal implementation target
until OpenAI publishes a stable public contract for it. OpenAI documents
Responses HTTP/SSE streaming and Realtime WebSocket; Codex contains an internal
Responses WebSocket transport using `wss://.../v1/responses` and
`response.create` frames, but cai will not implement that as a default public
SDK path while the contract is undocumented. If cai later adds it, it should be
explicitly experimental and opt-in.

## Status

This project is under active implementation. The API surface is still expected
to change while the Responses, Conversations, streaming, tool, compaction, and
example layers settle.

## Design Bias

- C89 with POSIX features.
- CMake/Ninja/Make based build.
- OpenAI API key from explicit config, `.env`, or `OPENAI_API_KEY`.
- `.env` overrides the inherited environment when present.
- OpenRouter can be selected with `cai_client_config_use_openrouter()`, which
  uses `OPENROUTER_API_KEY` and `https://openrouter.ai/api/v1`.
- `lonejson` is the JSON layer. The build downloads the official
  `github.com/sa6mwa/lonejson` release `.h.gz` header artifact pinned by
  version and SHA-256; it does not use a sibling checkout.
- Large inputs and outputs should stream through lonejson spooling/source/sink
  paths rather than being materialized in memory.
- Production SDK calls should choose a model explicitly.
- Session auto-compaction is enabled by default and uses Responses
  server-side compaction.
- Local spooled session history is opt-in with `enable_local_history`; default
  sessions rely on OpenAI server-side context through `previous_response_id` or
  Conversations. When enabled, `cai_session_export_history_source` streams the
  captured history as a JSON array for manual compaction, export, or offline
  inspection, and `cai_session_import_history_source` can load that JSON array
  back into a local-history-enabled session.
- Examples and integration development tests default to `gpt-5-nano`.
- OpenRouter development can use
  `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES`, currently
  `nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free`, based on OpenRouter's
  registry metadata.

## Agent Instructions

The preferred high-level API is method-style handles:

```c
cai_client_open(&client_config, &client, &error);
agent_config.prompt_cache_key = "my-app:assistant:v1";
client->new_agent(client, &agent_config, &agent, &error);

agent->add_user_text(agent, "Explain epoll in one paragraph.", &error);
agent->stream_text(agent, sink, &error);

agent->close(agent);
client->close(client);
```

Agent-level calls lazily create a default session and reuse it for follow-up
turns, so simple chat and workflow drivers get Responses context continuity
without manually carrying a `cai_session`. Explicit sessions remain available
for multi-session, conversation-handle, and advanced workflows.

Process restart/resume has two separate pieces by design. To continue
inference against OpenAI-held Responses context, persist either
`cai_session_previous_response_id(session)` or
`cai_session_conversation_id(session)` and restore it into a new session with
`cai_session_set_previous_response_id` or `cai_session_set_conversation_id`.
That is the actual server-side continuation handle. Opt-in local history
export/import is for offline inspection, experimental manual compaction, and
host-owned replay state; by itself it is not a substitute for the OpenAI
continuation id.

For applications that want one file/object to persist, use
`cai_session_export_state_source` and `cai_session_import_state_source`. The
state envelope is versioned JSON and contains the active continuation handle
plus local history when `enable_local_history` is on. Import restores the
continuation handle in all sessions and restores local history only for
local-history-enabled sessions. `cai_session_save_state_path` and
`cai_session_load_state_path` are the file-backed convenience helpers around
the same source/sink state envelope.

Set `prompt_cache_key` on `cai_agent_config` when an agent has a stable prompt
prefix, large developer instructions, or stable tool schemas. cai sends it on
every Responses request created by that agent so OpenAI can bucket similar
requests for prompt caching. The default is unset; cai does not generate random
cache keys because that would fragment the cache.

The high-level agent facade uses `developer_instructions`, matching the current
Responses API guidance around application-provided behavior. OpenAI documents
the top-level `instructions` field as high-level instructions that take
priority over `input`, and shows it as roughly equivalent to a `developer`
message. cai therefore treats these as developer instructions in its DX surface.

The agent/session facade does not expose a normal `system` prompt layer. Root
instructions are OpenAI/model policy and are not API-settable. The Responses
wire format still has low-level message roles, but normal cai users should set
`developer_instructions` on the agent and add user input with
`cai_session_add_user_text` or `cai_session_add_user_image_url`.

`assistant` is not an instruction surface. In the Responses API it represents
model-generated messages, and it is mainly useful when manually reconstructing
conversation history. cai's default sessions use `previous_response_id`, and
conversation sessions use OpenAI Conversations, so assistant turns are preserved
through those handles instead of being manually appended by application code.

Structured output text can be decoded into a caller-owned lonejson struct with
`cai_output_write_json`. The function parses `cai_output_text(output)` as JSON;
it does not inspect arbitrary response metadata or raw response JSON.

Tools are agent capabilities. The default `register_tool` path is a typed
lonejson callback; `register_raw_tool` is the JSON escape hatch. The typed path
uses a lonejson map for parameters and a second lonejson map for the result.
JSON Schema is derived from the parameter map automatically, and `_REQ` fields
become required schema fields:

```c
typedef struct lookup_customer_params {
  char *customer_id;
  long long limit;
} lookup_customer_params;

typedef struct lookup_customer_result {
  char *summary;
} lookup_customer_result;

int lookup_customer(void *ctx, const void *params, void *result,
                    cai_error *error) {
  const lookup_customer_params *in = params;
  lookup_customer_result *out = result;

  out->summary = cai_tool_result_strdup("customer found", error);
  return out->summary != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

agent->register_tool(agent, "lookup_customer", "Look up a customer.",
                     &lookup_customer_params_map,
                     &lookup_customer_result_map, lookup_customer, ctx,
                     &error);
```

For large typed tool results, callbacks can put source-backed or spooled fields
into their result struct instead of building a raw JSON string. Use
`cai_tool_result_set_source_path` for lonejson source fields and
`cai_tool_result_set_spooled` for lonejson spooled string/base64 fields; cai
streams the resulting JSON into the tool output sink and then cleans up those
handles.

`cai_tool_schema_from_map` can be used when callers want to inspect or enrich
the generated schema. Metadata helpers such as `describe` update existing
properties; they do not decide requiredness. Requiredness belongs in the
lonejson field map so the C decoder and OpenAI schema cannot drift.

```c
cai_tool_schema_from_map(&lookup_customer_params_map, &schema, &error);
schema->describe(schema, "customer_id", "Customer id", &error);
```

## OpenAI API Caveats

This SDK has to compensate for gaps in the OpenAI API contract. These are not
minor aesthetic problems; they directly affect whether a serious low-level SDK
can be correct, efficient, and pleasant to use.

The OpenAI Responses API is too untyped for a systems-language SDK. The OpenAPI
schema contains many broad unions, polymorphic items, open-ended maps, and raw
JSON escape hatches. That shape may be convenient for fast API evolution and
dynamic-language clients, but it is a poor contract for C. A C SDK needs stable
discriminators, bounded variant sets where possible, explicit ownership rules,
and mechanically reliable request/response shapes. Where the API does not
provide that, `cai` has to add typed wrappers for the common path and keep raw
JSON escape hatches only where typing would be dishonest or wasteful.

Model metadata is the larger design failure. Useful client behavior depends on
model facts:

- context window size,
- maximum output tokens,
- endpoint compatibility,
- streaming support,
- tool and structured-output support,
- image/audio support,
- deprecation state,
- safe auto-compaction thresholds.

The OpenAI Models API does not expose that as a real model registry. It exposes
basic model objects such as `id`, `object`, `created`, and `owned_by`, which can
answer whether a model exists for a key but not what the SDK needs to know to
run well. The richer data lives in documentation and model comparison pages,
not in a proper machine-readable registry.

That is bad API design. Context windows and capability flags are not marketing
copy; they are operational metadata. OpenAI does support server-side
compaction through `context_management` and `compact_threshold`, but the
threshold is an absolute token count, not a percentage. Without model context
metadata, a client cannot choose a principled default threshold such as 80% of
the context window, cannot report context-window usage as a percentage, cannot
validate whether a requested feature is supported, and cannot produce good
local diagnostics before an API call fails remotely.

As a result, `cai` will treat model metadata as curated SDK data:

- Model constants are strings, not a restrictive enum.
- Unknown model strings are allowed by default so new OpenAI models are usable
  before `cai` is updated.
- Known models get a compiled metadata table with capability flags and context
  limits plus pricing metadata where pricing has been explicitly entered.
- Metadata rows must distinguish verified, incomplete, inferred, deprecated,
  and unknown data.
- Runtime `/v1/models/{model}` checks may be used for availability, but not for
  context windows or feature discovery because the API does not return those
  facts.
- Auto-compaction must require known context metadata or an explicit caller
  token threshold. It should not guess.

In practice this means `cai` will likely need a reviewed generated metadata file
built from official OpenAI documentation/model comparison data. That is
unfortunate, but it is the only way to make default server-side
auto-compaction, context usage reporting, and feature validation useful until
OpenAI ships a proper model registry.

Relevant OpenAI documentation:

- <https://platform.openai.com/docs/api-reference/models/list>
- <https://platform.openai.com/docs/models/compare>
- <https://platform.openai.com/docs/api-reference/responses>

## Integration Tests

The default test suite is offline. Integration tests intentionally spend API tokens and
must be run explicitly:

```sh
build/integration/cai_integration_tests
CAI_INTEGRATION_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_STATE_RESTORE=1 build/integration/cai_integration_tests
```

`CAI_INTEGRATION_E2E=1` runs a 20-turn session regression against the real Responses
API using `gpt-5-nano` by default. It checks every turn for the current secret,
the first-turn secret, and the previous-turn secret so the test fails if
session continuity breaks.

`CAI_INTEGRATION_STATE_RESTORE=1` runs a shorter save/restore regression: it
teaches one exact key, saves the session state to disk, creates a fresh
session, loads that state, and verifies the next API turn can recall the key
through the restored continuation handle.

The e2e path enforces a local estimated spend cap using actual token usage and
compiled model pricing metadata. The default cap is `$0.02`; override it with:

```sh
CAI_INTEGRATION_SPEND_LIMIT_USD=0.05 CAI_INTEGRATION_E2E=1 build/integration/cai_integration_tests
```

OpenAI exposes organization-level Usage and Costs APIs, but those are not a
simple per-secret-key spend-limit API and may require admin-scoped credentials.
For the integration regression gate, cai therefore treats local estimated spend as the
hard stop and uses OpenAI billing/cost APIs only as future optional telemetry.
