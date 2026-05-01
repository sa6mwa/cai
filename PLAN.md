# cai implementation plan

This document turns `SPEC.md` into an implementation plan for `cai`, a C89
SDK-style OpenAI API client focused on the Responses API, Conversations, and
both Responses and Realtime WebSocket workflows.

The goal is not to expose libcurl, raw JSON, or transport primitives as the
main user experience. The public API should be a small, handle-oriented C SDK
with clear ownership, explicit model selection, synchronous tool callbacks for
the first version, and predictable diagnostics through pslog.

The primary downstream integration target is Vectis (`../vectis/`): a
Kore-backed REST API service runtime that already treats `lonejson`, `lc_source`,
lockd queues/state, curl, pslog, and Lua as first-class workflow building
blocks. `cai` must fit that style: stream request/response data where possible,
avoid avoidable transient allocations, and make agentic workflows easy to drive
from a C or Lua route handler without leaking transport machinery into the
application.

## Current decisions

- Language and platform: C89 with POSIX features, built with CMake/Ninja/Make.
- Dependency source: cai consumes lonejson directly from the official
  `github.com/sa6mwa/lonejson` release `.h.gz` asset, pinned by version and
  SHA-256 in CMake. Do not use a sibling lonejson checkout as an implicit
  dependency. Other native dependencies should come from the normal platform or
  cai release provisioning path.
- JSON: all API JSON construction/parsing goes through lonejson.
- Logging: pslog is accepted as a borrowed host-owned logger in client config,
  with `logger_disabled` as the zero-default opt-out.
- API coverage target: full OpenAI Responses API surface, including newer
  Conversations endpoints.
- WebSocket coverage target: both Responses WebSocket mode and Realtime
  WebSocket.
- MCP: not part of the first implementation, except that the C SDK design must
  not paint us into a corner for later MCP support.
- Tool execution: synchronous local C callbacks only in the first version.
- Default SDK model: no hidden default for production SDK calls. Callers must
  choose a model explicitly.
- Development/integration-test model: `gpt-5-nano`, unless the integration
  environment rejects it. This is the default for examples and integration
  tests because it is the cheapest currently intended development model.
- Session auto-compaction defaults to enabled and uses Responses server-side
  compaction by sending `context_management` with a resolved
  `compact_threshold` token count. Callers opt out with
  `disable_auto_compaction`.
- Local spooled session history is disabled by default. Callers opt in with
  `enable_local_history` when they need experimental manual compaction,
  session export, or offline history inspection. Export is exposed as a
  `cai_source` over a JSON array so callers can stream it instead of
  materializing it. Import accepts the same JSON array back into a
  local-history-enabled session for host-owned replay/offline state.
- Session resume is explicit: callers persist either the last
  `previous_response_id` or an OpenAI Conversation ID and restore it into a new
  session. That is the server-side continuation path; local history import is
  not treated as model context unless a caller deliberately uses lower-level
  replay/manual-compaction flows.
- A versioned session state envelope is available as a `cai_source` so hosts can
  persist one JSON document containing the active continuation handle and, when
  enabled, local spooled history. File-backed save/load helpers wrap the same
  state envelope for the common disk persistence path.
- cai depends directly on the official lonejson release and uses
  `lonejson_spooled_append` for append-heavy history/tool-output paths.
- Unit tests never hit OpenAI. Integration tests require explicit opt-in.
- `.env` loading precedence: if `.env` exists, load `OPENAI_API_KEY` from it
  and let it override the process environment. If `.env` does not exist, use
  the inherited `OPENAI_API_KEY`.
- Single-header distribution is not a primary goal. `cai` is too broad for a
  lonejson/libpslog-style single-header implementation to be the architectural
  center. Ship a normal library with installed headers; consider a declarations
  convenience header or generated amalgamation only after the core stabilizes.
- Streaming and low-allocation operation are design requirements, not
  afterthoughts. Convenience helpers may allocate, but route-handler and worker
  workflows must have streaming alternatives.

## Product shape

`cai` should feel like a C SDK, not a transport wrapper. The API exposes
handle structs with method-style function pointers plus equivalent free
functions for wrapper friendliness. That keeps the common DX compact while
still making ownership and state explicit:

- `cai_client`: owns shared configuration, dependency handles, base URLs,
  auth, logger, allocator, and transport defaults.
- `cai_agent`: optional higher-level facade over model, instructions, tools,
  and default generation settings.
- `cai_session`: SDK-level stateful workflow handle. It should hide the choice
  between `previous_response_id` chaining and OpenAI Conversation IDs where that
  improves DX.
- `cai_conversation`: explicit handle for the OpenAI Conversations endpoints.
- `cai_response`: owned parsed response object with helpers for status, usage,
  text output, tool calls, and raw JSON escape hatches.
- `cai_stream`: synchronous event stream handle/callback runner for SSE and
  WebSocket events.
- `cai_realtime_session`: Realtime WebSocket session facade.
- `cai_tool_registry`: synchronous function-tool callback registry.
- `cai_output`: streaming result facade that can feed lonejson sinks,
  `lc_source` consumers, files, or application callbacks without forcing a
  whole response body into memory.
- `cai_workflow`: optional orchestration helper for multi-agent flows where the
  host still owns branching decisions, persistence, and external side effects.

The implementation should keep lower-level HTTP, SSE, WebSocket, JSON, and
tool-loop machinery internal unless a narrow escape hatch is needed.

## Target integration model

The canonical serious use case is a Vectis route handler or worker that:

1. Streams the inbound Kore request body into a lonejson-mapped request struct.
2. Builds one or more `cai_agent` instances with explicit instructions, model,
   tools, and input attachments.
3. Seeds a session with text, images, and context from the request, lockd state,
   queue message, or another `lc_source`.
4. Runs one or more agent turns.
5. Branches in C with `if`/`switch`, or delegates judgment to another agent.
6. Streams the final output into a lonejson response writer, an `lc_source`, a
   lockd state object, a lockd queue message, or a downstream curl request.

The SDK should make the simple path compact while preserving host control:

```c
static vectis_status handle_request(vectis_request *req,
                                    vectis_response *res,
                                    void *ctx) {
  struct my_input input;
  cai_agent *agent;
  cai_session *session;
  cai_output *output;
  cai_error error;

  vectis_request_json_into(req, &my_input_map, &input);

  app->cai->new_agent(app->cai, &support_agent_config, &agent, &error);
  agent->add_user_text(agent, input.question, &error);
  agent->add_user_image_url(agent, input.image_url, NULL, &error);
  agent->run_output(agent, &output, &error);
  cai_output_as_lc_source(output, &source, &error);

  return vectis_response_json_source(res, 200, source, &my_output_map);
}
```

That example is intentionally approximate; the important point is the dataflow:
the host owns the web request, JSON schema, lockd/curl side effects, and control
flow, while `cai` owns OpenAI request/response mechanics and agent/session
state.

## Example applications

Examples should be organized as one directory per example under `examples/`.
They are part of the SDK's DX contract, not throwaway demos.

Required examples:

- `basic-response`: minimal non-streamed Responses call.
- `streaming-text`: direct streaming text call with token-by-token terminal
  output.
- `terminal-chat`: interactive chat that transparently preserves Responses
  turn context, prints usage metadata, supports `/quit`, `/exit`, EOF, and uses
  `gpt-5-nano` for development.
- `conversation-handles`: explicit conversation-handle construction and reuse
  without exposing callers to manual ID plumbing in normal flows.
- `mike-mind`: heavy self-contained knowledge-base chatbot built from the full
  Mike Mind skill corpus.

The `mike-mind` example must be self-contained at runtime. It should not read
external checkout paths, require a file tool, or ask the model to inspect
paths. Instead, the implementation should embed the full Mike Mind skill
material into the prompt/corpus shipped with the example. "Full" here is
intentional: this is meant to fill large context windows and exercise cai as a
heavy knowledge-base chatbot, not to be a summary or small representative
sample.

The generated Mike Mind prompt should:

- include clear developer instructions inferred from `SKILL.md`,
- append the complete referenced corpus needed by the skill,
- tell the model to synthesize from the embedded corpus rather than behave like
  a document lookup tool,
- avoid runtime file references unless the answer is explicitly pointing a
  human at public follow-up material,
- keep the chatbot interface identical to normal session usage so large prompts,
  auto-compaction, usage reporting, and streaming behavior are exercised through
  public cai APIs.

Implementation note: this can be a committed generated C include file or another
repo-native artifact that CMake compiles into the example. If a generator is
added, it should produce deterministic output and be documented, but the example
binary must not depend on the external skill directory at runtime.

## Allocation and streaming policy

`cai` should have two API tiers:

- Convenience tier: returns owned strings and parsed response objects for simple
  CLIs, tests, and small services.
- Streaming tier: lets hosts provide sinks/sources/callbacks so large inputs,
  images, generated JSON, tool outputs, and final response bodies do not need to
  be fully materialized.

Rules:

- Request input should accept borrowed strings, `lc_source`, file paths, and
  application callbacks.
- Response output should be representable as:
  - parsed metadata plus streamed content events,
  - a generated lonejson writer/source,
  - an `lc_source`-compatible byte source,
  - an application callback sink.
- Use fixed-capacity or caller-provided buffers where practical for hot paths.
- Avoid hidden global state. A Vectis worker process may own multiple clients,
  agents, and sessions.
- Do not allocate just to bridge between lonejson and `lc_source` when a
  callback bridge can stream.
- Any helper named like `*_text()` or `*_string()` may allocate, but the docs
  must mark it as a convenience path.
- Tool callbacks should be able to stream their output, not only return a
  heap-allocated string, even if string output is the first implemented result
  type.

## Proposed public API direction

Names and field lists will change during implementation, but this is the API
shape to iterate from.

```c
typedef struct cai_client cai_client;
typedef struct cai_agent cai_agent;
typedef struct cai_session cai_session;
typedef struct cai_conversation cai_conversation;
typedef struct cai_response cai_response;
typedef struct cai_stream cai_stream;
typedef struct cai_output cai_output;
typedef struct cai_source cai_source;
typedef struct cai_sink cai_sink;
typedef struct cai_error cai_error;

typedef enum cai_status {
  CAI_OK = 0,
  CAI_ERR_INVALID = 1,
  CAI_ERR_NOMEM = 2,
  CAI_ERR_TRANSPORT = 3,
  CAI_ERR_PROTOCOL = 4,
  CAI_ERR_SERVER = 5,
  CAI_ERR_CANCELLED = 6
} cai_status;
```

### Client configuration

```c
typedef struct cai_client_config {
  const char *api_key;              /* optional; env/.env fallback */
  const char *base_url;             /* default https://api.openai.com/v1 */
  const char *organization_id;      /* optional OpenAI-Organization */
  const char *project_id;           /* optional OpenAI-Project */
  long timeout_ms;
  int http_2_disabled;
  int insecure_skip_verify;
  size_t json_response_limit_bytes;
  pslog_logger *logger;             /* borrowed */
  int logger_disabled;
  cai_allocator allocator;
} cai_client_config;

void cai_client_config_init(cai_client_config *config);
int cai_client_open(const cai_client_config *config, cai_client **out,
                    cai_error *error);
void cai_client_close(cai_client *client);
```

The client constructor performs `.env` resolution unless `api_key` is provided
explicitly. Explicit `api_key` always wins over `.env` and process environment
because it is a direct API call argument.

All string fields and `logger` are borrowed for the duration of
`cai_client_open()`. The client copies strings it needs to retain. The logger is
borrowed and not destroyed by CAI; when `logger_disabled` is nonzero, the client
stores no logger even if `logger` is set.

### Source and sink interop

`cai` should not make `liblockdc` a conceptual requirement for all users, but it
can interoperate with `lc_source` and `lc_sink` when liblockdc headers are
available through the SDK bundle.

```c
typedef size_t (*cai_source_read_fn)(void *context, void *buffer,
                                     size_t count, cai_error *error);
typedef int (*cai_source_reset_fn)(void *context, cai_error *error);
typedef void (*cai_source_close_fn)(void *context);

typedef int (*cai_sink_write_fn)(void *context, const void *bytes,
                                 size_t count, cai_error *error);
typedef void (*cai_sink_close_fn)(void *context);

int cai_source_from_callbacks(const cai_source_callbacks *callbacks,
                              cai_source **out, cai_error *error);
int cai_source_from_lc(struct lc_source *source, cai_source **out,
                       cai_error *error);
int cai_output_as_lc_source(cai_output *output, struct lc_source **out,
                            cai_error *error);
int cai_output_write_json(cai_output *output, const lonejson_map *map,
                          void *value, cai_error *error);
int cai_sink_stdout(cai_sink **out, cai_error *error);
int cai_sink_stderr(cai_sink **out, cai_error *error);
```

The exact function names can change, but the boundary must exist. Vectis should
be able to hand request bodies, image payloads, lockd state, and generated
outputs across the cai boundary without converting everything through temporary
heap strings.

The stdout/stderr sink helpers are convenience APIs for pipe-style CLI usage,
examples, and quick tools. They should behave like normal `cai_sink` instances
backed by `FILE *` writes, report write errors through `cai_error`, and avoid
forcing callers to define trivial sink callback structs just to stream tokens to
the terminal.

### Models

Expose model constants as strings, not a restrictive enum, because OpenAI model
availability changes over time and users may need newly released IDs before a
new `cai` release.

```c
#define CAI_MODEL_GPT_5_NANO "gpt-5-nano"
#define CAI_MODEL_GPT_5_MINI "gpt-5-mini"
#define CAI_MODEL_GPT_5 "gpt-5"
```

Plan:

- Seed `include/cai/models.h` with documented model IDs that support
  `v1/responses`.
- Annotate models with endpoint/tool capability metadata in generated or
  table-driven code: Responses, Realtime, streaming, function calling,
  structured output, image input, audio support, tool support.
- Treat model metadata as explicitly sourced data, not guessed truth. The
  OpenAI Models API exposes model availability and basic identity fields, but
  it is not currently a complete capability registry for context windows,
  endpoint support, tool support, max output tokens, or automatic compaction
  thresholds.
- Keep `cai` permissive for unknown model strings. Unknown models may be used,
  but helpers should report that local metadata is unavailable.
- Do not prevent callers from passing an arbitrary model string.
- Provide `cai_model_info()` and `cai_model_supports()` helpers for local
  validation and better errors.
- Treat unknown model strings as allowed by default, with optional strict
  validation mode.

Metadata strategy:

- The compiled metadata table is the SDK's fast local answer for DX, validation,
  examples, and auto-compaction thresholds.
- Every metadata row should carry enough information for cai to distinguish
  verified metadata from unknown or incomplete metadata. This can be a flags
  field, an enum, or separate helpers such as `cai_model_has_context_window()`.
- Runtime model lookup should be optional and should only answer what the
  OpenAI API actually exposes: whether a model is visible to the API key and
  its basic model object fields. It should not pretend to discover missing
  capability data if the platform does not return that data.
- A future generated metadata update step may scrape or ingest official model
  documentation/OpenAPI metadata, but generated output must be reviewed and
  committed. Runtime SDK startup should not depend on scraping docs.
- Auto-compaction uses OpenAI server-side compaction on normal Responses create
  requests. `compact_threshold` is an absolute token count, so cai resolves it
  from `compact_threshold_tokens`, or from `compact_threshold_percent`
  multiplied by known model context-window metadata. The default percentage is
  80. If auto-compaction is enabled for a model whose context window is unknown,
  cai should return an actionable configuration error unless the caller supplies
  an explicit token threshold.

### Responses

The raw Responses API surface should be represented without forcing users to
manually build JSON:

```c
typedef struct cai_response_create_params cai_response_create_params;

void cai_response_create_params_init(cai_response_create_params *params);
int cai_response_create_params_set_model(cai_response_create_params *params,
                                         const char *model);
int cai_response_create_params_add_text(cai_response_create_params *params,
                                        const char *role,
                                        const char *text);
int cai_response_create_params_add_image_url(cai_response_create_params *params,
                                             const char *role,
                                             const char *url,
                                             const char *detail);
int cai_response_create_params_set_instructions(
    cai_response_create_params *params, const char *instructions);
int cai_response_create_params_set_previous_response_id(
    cai_response_create_params *params, const char *response_id);
int cai_response_create_params_set_conversation_id(
    cai_response_create_params *params, const char *conversation_id);

int cai_client_create_response(cai_client *client,
                               const cai_response_create_params *params,
                               cai_response **out, cai_error *error);
int cai_client_stream_response(cai_client *client,
                               const cai_response_create_params *params,
                               const cai_stream_handler *handler,
                               cai_error *error);
int cai_client_retrieve_response(cai_client *client, const char *response_id,
                                 cai_response **out, cai_error *error);
int cai_client_delete_response(cai_client *client, const char *response_id,
                               cai_error *error);
int cai_client_cancel_response(cai_client *client, const char *response_id,
                               cai_response **out, cai_error *error);
```

Response helpers should cover common DX needs:

- `cai_response_id()`
- `cai_response_status()`
- `cai_response_output_text()`
- `cai_response_usage()`
- `cai_response_tool_call_count()`
- `cai_response_tool_call_at()`
- `cai_response_raw_json()`
- `cai_response_destroy()`

Streaming alternatives must exist for the same output:

- `cai_response_output_source()`
- `cai_response_write_output_json()`
- `cai_response_each_output_item()`
- `cai_response_each_event()`

### Conversations

Expose OpenAI conversation objects directly, but let sessions use them
internally where convenient.

```c
int cai_client_create_conversation(cai_client *client,
                                   const cai_conversation_create_params *params,
                                   cai_conversation **out, cai_error *error);
int cai_client_retrieve_conversation(cai_client *client, const char *id,
                                     cai_conversation **out, cai_error *error);
int cai_client_update_conversation(cai_client *client, const char *id,
                                   const cai_conversation_update_params *params,
                                   cai_conversation **out, cai_error *error);
int cai_client_delete_conversation(cai_client *client, const char *id,
                                   cai_error *error);
int cai_conversation_create_item(cai_conversation *conversation,
                                 const cai_item_params *params,
                                 cai_error *error);
int cai_conversation_list_items(cai_conversation *conversation,
                                cai_item_list **out, cai_error *error);
```

### Sessions and agents

The recommended DX should start here for normal users:

```c
cai_agent_config agent_config;
cai_agent_config_init(&agent_config);
agent_config.model = CAI_MODEL_GPT_5_NANO;
agent_config.developer_instructions = "You are concise.";

client->new_agent(client, &agent_config, &agent, &error);
agent->send_text(agent, "Explain epoll in one paragraph.", &response, &error);
```

Decision:

- `cai_session` should use `previous_response_id` by default because it is the
  smallest state model and maps cleanly to Responses.
- Agent-level calls should lazily create and reuse a default session, making
  simple flows context-preserving without explicit session plumbing.
- If a session is created with `use_conversation=1`, it should create or attach
  an OpenAI Conversation ID and send future turns through that conversation.
- This keeps the default simple while preserving explicit conversation support.

For workflow-heavy hosts, the session facade should support incremental setup
and streaming run output:

```c
agent->new_session(agent, &session, &error);
session->add_user_text(session, user_text, &error);
session->add_user_image_url(session, image_url, "high", &error);
session->stream_text(session, sink, &error);
session->run_output(session, &output, &error);
```

For multi-channel streaming, `stream_text` remains answer-text only. Callers
that want reasoning summaries or later event channels should pass explicit
sinks:

```c
cai_stream_sinks sinks;
cai_stream_sinks_init(&sinks);
sinks.reasoning_summary = reasoning_sink;
sinks.output_text = answer_sink;
sinks.reasoning_summary_prefix.text = "[reasoning] ";
sinks.reasoning_summary_suffix.text = "\n\n";
sinks.output_text_prefix.text = "[response] ";
session->stream(session, &sinks, &error);
```

Prefix/suffix affixes are optional and can be static strings or callbacks. They
are emitted by the streaming facade around the first/last chunk of each channel,
so examples and CLIs can format interleaved stream channels without custom sink
state. When affixes are unset, `cai` forwards the model deltas unchanged.

The agent/session facade intentionally exposes developer instructions and user
inputs, not arbitrary role strings. OpenAI's current Responses guidance treats
`instructions` as high-level developer behavior and shows `developer` messages
as the equivalent message-role form. `assistant` messages remain a low-level
transport concept for manually replaying prior model output when callers are not
using `previous_response_id` or Conversations. Normal cai sessions preserve
assistant turns transparently through those server-side state handles.

Tool registration is agent-level:

```c
agent->register_tool(agent, "lookup_customer", "Look up a customer.",
                     &lookup_customer_params_map,
                     &lookup_customer_result_map, lookup_customer_cb, ctx,
                     &error);
```

`register_tool` is the typed lonejson path. It derives JSON Schema from the
parameter map and serializes the handler result through the result map. Required
fields come from lonejson `_REQ` fields so the C decoder and OpenAI schema have
one source of truth. `register_raw_tool` is the explicit escape hatch for
dynamic JSON parameters.

Multi-agent workflows should remain host-driven. `cai` should make it cheap to
construct several agents and sessions, but it should not impose a graph runtime
or hidden planner in the first version. The host should be able to do:

- main agent generates a proposed answer,
- judge agent evaluates or selects a branch,
- repair agent rewrites or asks for another tool call,
- host persists intermediate state to lockd or enqueues follow-up work.

An optional `cai_workflow` helper may be added later if repeated patterns emerge
from examples, but the first implementation should keep orchestration explicit.

### Tool callbacks

First version supports synchronous local function tools:

```c
typedef int (*cai_tool_fn)(void *context,
                           const void *params,
                           void *result,
                           cai_error *error);

int cai_tool_registry_register_lonejson(
    cai_tool_registry *registry,
    const char *name,
    const char *description,
    const struct lonejson_map *params_map,
    const struct lonejson_map *result_map,
    cai_tool_fn callback,
    void *context,
    cai_error *error);
```

Tool execution loop:

1. Send response request with registered function tool schemas.
2. Parse completed response or stream events.
3. For each function call, parse arguments into the parameter map and invoke
   the registered synchronous callback.
4. Serialize the result map as the `function_call_output` payload with the
   matching `call_id`.
5. Continue until the response completes without pending local tool calls or an
   error occurs.

Initial result payload support:

- Required: lonejson-mapped JSON object output, including dynamic strings.
- Early follow-up: source/spooled fields for large text/file-like tool outputs.
- Required in the API design even if implemented after string output:
  source-backed tool output so a C or Lua callback can stream data generated
  from lockd, files, or downstream APIs.

### Streaming and WebSockets

SSE streaming and Responses WebSocket events should share the same semantic
event parser and callback shape where possible:

```c
typedef int (*cai_stream_event_fn)(void *context,
                                   const cai_stream_event *event,
                                   cai_error *error);

typedef struct cai_stream_handler {
  cai_stream_event_fn on_event;
  void *context;
} cai_stream_handler;
```

Responses WebSocket mode:

- Connect to `wss://api.openai.com/v1/responses`.
- Send `response.create` events whose payload mirrors Responses create, except
  transport-only fields such as `stream` and `background`.
- Support `previous_response_id` continuation and incremental input.
- Support `generate=false` warmup after the core path works.

Realtime WebSocket:

- Separate `cai_realtime_session` facade, because Realtime has different
  models, event types, audio behavior, and session lifecycle.
- First Realtime milestone should support text in/text out and synchronous
  function calls.
- Audio streaming can follow after the text/tool path is stable.

## Internal architecture

Suggested source layout:

```text
include/cai/cai.h
include/cai/models.h
src/cai_client.c
src/cai_env.c
src/cai_error.c
src/cai_http.c
src/cai_json.c
src/cai_models.c
src/cai_response.c
src/cai_conversation.c
src/cai_agent.c
src/cai_session.c
src/cai_tools.c
src/cai_stream_sse.c
src/cai_ws_responses.c
src/cai_ws_realtime.c
src/cai_log.c
src/cai_allocator.c
src/cai_internal.h
lua/
tests/
mock/
cmake/
scripts/
```

Boundary rules:

- Public headers expose opaque handles and stable structs only.
- JSON schema details stay in `src/cai_json.c` and endpoint-specific modules.
- HTTP and WebSocket transport stay internal.
- Agent/session logic calls public-ish internal response/conversation
  primitives, not curl directly.
- Lua binding wraps the C SDK surface, not internal transport.

## Testing strategy

### Unit tests

Unit tests must be offline and deterministic:

- `.env` precedence parser.
- Model capability lookup.
- Request JSON serialization.
- Response JSON parsing.
- API error parsing.
- SSE event framing and event parsing.
- WebSocket event JSON parsing independent of network.
- Tool-loop state machine.
- Ownership and cleanup behavior.

### Mock integration tests

Add a repo-local mock OpenAI service written in C:

- Lives under `mock/`.
- Built by CMake.
- Runs inside `docker-compose.yaml` for integration tests.
- Implements enough HTTP endpoints and WebSocket behavior to verify cai:
  - `POST /v1/responses`
  - `GET /v1/responses/{id}`
  - `DELETE /v1/responses/{id}`
  - `POST /v1/responses/{id}/cancel`
  - `GET /v1/responses/{id}/input_items`
  - conversation CRUD and item endpoints
  - SSE streaming responses
  - Responses WebSocket mode
  - Realtime WebSocket text/tool subset
- No extra implementation-language dependency for the mock server.

The mock server can use POSIX sockets directly. TLS is not required for the
first mock milestone if the SDK supports configurable `http://` base URLs for
tests.

### Integration tests

Integration tests hit OpenAI only when explicitly enabled:

```sh
CAI_ENABLE_INTEGRATION_TESTS=1 make test-integration
```

Integration tests:

- Resolve API key through the same `.env` precedence rules as the SDK.
- Use `CAI_TEST_MODEL` if set, otherwise `gpt-5-nano`.
- Keep prompts tiny and deterministic.
- Never run from default `make test`.

## Build and release plan

Mirror liblockdc where practical:

- `CMakeLists.txt`, `CMakePresets.json`, and `Makefile` with familiar targets.
- Host debug, ASan/UBSan, coverage, release, and cross presets.
- Direct pinned download of the official lonejson release header artifact.
- Release/dependency provisioning for other native libraries should remain
  explicit and reproducible; do not silently use sibling checkouts.
- Release matrix:
  - `x86_64-linux-gnu`
  - `x86_64-linux-musl`
  - `armhf-linux-gnu`
  - `armhf-linux-musl`
  - `aarch64-linux-gnu`
  - `aarch64-linux-musl`
  - `arm64-apple-darwin`
- `make release` writes tarballs under `dist/`.
- If HEAD has a `vX.Y.Z` tag, use that version for archive names and version
  macros.
- Archives use `tar --owner=0 --group=0`.
- Build LuaRock artifacts under `dist/` once the C API is stable enough to
  wrap.

## Implementation milestones

### Milestone 0: project skeleton

- Add CMake/Makefile/scripts based on liblockdc conventions.
- Add public header skeleton, version header generation, pkg-config metadata.
- Add dependency fetch/use logic for the official lonejson release `.h.gz`
  artifact and keep it pinned by checksum.
- Add C89 compile flags and clang-format.
- Add minimal tests for build metadata and public header C-only inclusion.

### Milestone 1: core client and offline JSON

- Implement allocator, error, logger, `.env`, auth header, and base URL config.
- Implement `cai_source`, `cai_sink`, and `lc_source` interop shims.
- Implement request JSON generation for basic `responses.create`.
- Implement response parsing for IDs, status, output text, errors, and usage.
- Implement output metadata/content separation so convenience strings and
  streaming output share the same parsed event model.
- Unit-test serialization/parsing exhaustively with fixtures.

### Milestone 2: HTTP Responses API

- Implement non-streamed Responses endpoints.
- Implement Conversations endpoints.
- Implement count/list input items if current docs expose them.
- Keep standalone `/responses/compact` as experimental lower-level API
  coverage; normal sessions should prefer server-side compaction through
  `context_management`.
- Add mock HTTP server path for deterministic integration tests.

### Milestone 3: agent/session DX

- Implement `cai_agent` and `cai_session`.
- Default session chain uses `previous_response_id`.
- Default auto-compaction sends `context_management` with a resolved
  `compact_threshold`.
- Conversation-backed session mode is opt-in.
- Support incremental session setup with text, image URLs, image sources, tool
  registries, and host-owned context pointers.
- Add streaming `run_stream()` and `run_output()` paths before adding richer
  orchestration helpers.
- Add examples that demonstrate the intended DX.

### Milestone 4: tools

- Implement function tool registry.
- Serialize JSON schemas into Responses requests.
- Parse tool calls.
- Run synchronous callbacks.
- Submit `function_call_output`.
- Add source-backed tool result API. Large tool outputs should be represented
  with `lonejson_spooled` or source/sink plumbing and serialized back into the
  session without materializing the whole value in memory. Bounded buffering is
  only acceptable inside the normal chunk buffers of lonejson, curl, or the
  transport layer.
- Unit-test multi-tool and error paths.

### Milestone 5: SSE streaming

- Implement `stream=true` HTTP streaming.
- Parse semantic Responses events.
- Surface callback API.
- Support streaming tool-call arguments.

### Milestone 6: Responses WebSocket mode

- Implement WebSocket connect/send/receive for `wss://.../v1/responses`.
- Reuse event parser and tool loop.
- Support incremental input with `previous_response_id`.
- Add mock WebSocket tests.

### Milestone 7: Realtime WebSocket

- Implement Realtime text/tool subset.
- Keep Realtime facade separate from Responses sessions.
- Add audio-aware type placeholders but avoid full audio streaming until text
  and tool behavior is solid.

### Milestone 8: Lua binding

- Wrap client, response, agent/session, and basic tools.
- Add Lua tests and LuaRock packaging.
- Keep logger injection C-only for now as requested.

### Milestone 9: release matrix

- Complete cross-build scripts.
- Verify archives, checksums, pkg-config, CMake package metadata, and LuaRock.
- Add release verification tests modeled after liblockdc.

## Resolved decisions and remaining questions

Resolved:

- `.env` overrides the inherited process `OPENAI_API_KEY`. An explicit
  `cai_client_config.api_key` still wins because it is a direct API call
  argument, not ambient environment.
- Strict model validation defaults to off. Unknown model strings are accepted,
  while metadata helpers report unavailable local metadata.
- `cai_session` defaults to transparent `previous_response_id` chaining.
  Conversation-backed sessions are opt-in and should expose handles without
  forcing normal callers to manage IDs manually.
- Auto-compaction defaults to enabled and uses server-side Responses
  compaction. Standalone `/responses/compact` remains available only as an
  experimental lower-level/manual path for now.
- Local history capture is opt-in and independent of server-side
  auto-compaction.
- Integration tests and examples default to `gpt-5-nano`.
- Streaming means actual streaming. Large history, tool output, generated JSON,
  and final response data should use lonejson spooling/source/sink APIs instead
  of faux streaming through full in-memory materialization.
- cai uses native `lonejson_spooled_append` from the official lonejson release
  for append-heavy paths; no cai-side parser/rebuild shim should remain.

Remaining:

1. Is the mock OpenAI server allowed to start as plain HTTP for tests, with TLS
   coverage handled by libcurl/OpenSSL dependency smoke tests later?
2. Should Lua bindings wait until after the C agent/session API stabilizes, or
   should they track each milestone from the beginning?
3. What exact public shape should model metadata expose for verified,
   incomplete, and unknown capability data?

## Documentation sources checked

- OpenAI Responses API reference, including Responses and Conversations
  endpoint navigation.
- OpenAI Streaming guide for semantic Responses events.
- OpenAI WebSocket Mode guide for Responses WebSocket behavior.
- OpenAI Authentication reference for Bearer auth and optional organization /
  project headers.
- OpenAI model docs and model comparison pages for endpoint and feature
  compatibility.
