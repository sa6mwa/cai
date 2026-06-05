# cai

`cai` is a C89/POSIX SDK-style client for the OpenAI Responses API,
Conversations, and HTTP/SSE streaming workflows.

The goal is a small, handle-oriented C API that fits systems such as Vectis:
stream-first data flow, explicit ownership, predictable error handling, and
good ergonomics for agentic workflows without forcing application code to build
raw HTTP requests or hand-roll JSON.

See [ROADMAP.md](ROADMAP.md) for current prerelease status, parked work, and
future feature planning.

OpenAI documents both Responses WebSocket mode and Realtime WebSocket, but cai
does not implement either yet. Responses WebSocket is a transport option for
long-running Responses workflows; Realtime WebSocket is a separate Realtime API
surface for low-latency text/audio sessions. Both are parked in
[ROADMAP.md](ROADMAP.md) until cai has an explicit WebSocket transport slice,
a C mock WebSocket test server, and clear DX separate from the current HTTP/SSE
Responses path.

## Scope

The first prerelease target is the C SDK plus the Lua 5.5 facade: OpenAI
Responses, Conversations, HTTP/SSE streaming, agent/session DX, local tool
callbacks, MCP tool serving, examples, LuaRock packaging, and release
packaging.

WebSocket transports are not part of the first prerelease. They are tracked in
[ROADMAP.md](ROADMAP.md).

## Status

This project is in prerelease hardening. The C API is close to first-release
shape, but may still change before the initial tag.

The verification tiers are split intentionally:

- `make prerelease` runs the standard local gate: clang-format, debug tests,
  TSan, MSan smoke, fuzz smoke, Lua tests, and deterministic local example
  smoke.
- `make prerelease-live` runs the live-provider gate: integration tests plus a
  curated set of real non-interactive examples.
- `make prerelease-hardening` runs both tiers, then long fuzz and the release
  matrix.

## Design Bias

- C89 with POSIX features.
- CMake/Ninja/Make based build.
- Installed builds export CMake package targets under `lib/cmake/cai` and a
  relocatable `cai.pc` pkg-config file.
- `make release` builds/tests the release matrix and writes binary SDK
  archives as `dist/cai-<version>-<target>.tar.gz`, a source archive as
  `dist/cai-<version>.tar.gz`, and `dist/cai-<version>-CHECKSUMS`.
  Release verification checks archive roots, installed docs, pkg-config/CMake
  metadata, dependency-header exclusion, and host-free relative runpaths
  (`$ORIGIN` on ELF targets, `@loader_path` on Darwin).
- The deterministic local example smoke gate is intentionally narrow. It
  covers the examples Makefile and the MCP server example automatically. Live
  example execution is opt-in and limited to a curated non-interactive subset.
- Release versions are detected from an exact `vX.Y.Z` git tag. Local release
  candidates can use `CAI_VERSION_OVERRIDE=X.Y.Z-rc.N cmake ...` or the same
  override through a custom CMake preset; untagged builds intentionally fall
  back to `0.0.0`.
- Binary SDK archives contain installed headers, CMake/pkg-config metadata,
  `libcai.a`, and the versioned shared library with compatibility symlinks for
  the target platform. They also include `README.md` and `LICENSE` under
  `share/doc/libcai/`, plus supplemental docs under `share/doc/libcai/docs/`.
- Dependency mode defaults to `cpkt`: the build uses the official
  `github.com/sa6mwa/c.pkt.systems` release tarball for the selected target to
  provide curl, OpenSSL, nghttp2, libssh2, zlib, and the native dependency
  stack. It also uses official `lonejson` and `libpslog` release artifacts for
  the JSON library and logger header. Those dependency archives ship consumer
  CMake and pkg-config metadata, and cai consumes that metadata when available.
  Tarball URLs and SHA-256 values are pinned in CMake; sibling checkout
  artifacts are not dependency inputs.
- `CAI_DEPENDENCY_MODE=host` uses already-installed host dependencies instead:
  libcurl, OpenSSL crypto, `lonejson.h` plus `liblonejson`, and `pslog.h`.
  The discovered `liblonejson` must match cai's required ABI generation
  (`liblonejson.so.16` on Linux, `liblonejson.16.dylib` on Darwin). `auto`
  chooses host only when all required host pieces are discoverable and the
  lonejson ABI matches, otherwise it falls back to `cpkt`.
- Installed CMake and pkg-config metadata preserve that dependency mode.
  `cpkt` mode records the official `c.pkt.systems` dependency URL and checksum;
  `host` mode records the resolved host include/library paths. `cai` archives
  do not vendor dependency headers.
- `cai_client_open` resolves API keys only from explicit `config.api_key` or
  `getenv(config.api_key_env)`. If `api_key_env` is NULL, cai uses
  `OPENAI_API_KEY`. It does not implicitly load dotenv files.
- ChatGPT subscription auth can be supplied explicitly through
  `cai_chatgpt_auth` from `cai/auth.h`. Open a Codex-compatible `auth.json`
  path with `cai_chatgpt_auth_open`, set `config.chatgpt_auth`, and keep the
  auth handle alive for the client lifetime. If `auth_json_path` is NULL/empty,
  the auth library uses `$XDG_CONFIG_HOME/cai/auth.json`, or
  `$HOME/.config/cai/auth.json` when `XDG_CONFIG_HOME` is unset or relative.
  cai reads `id_token`, `access_token`, `refresh_token`, and `account_id` from
  the file, refreshes access tokens through the configured OAuth issuer before
  expiry, persists returned token fields, and retries one 401/403 response
  after a forced refresh. Before using a refresh token, cai re-reads the auth
  file so another process can refresh or replace same-account tokens without
  racing the stale in-memory handle; a different account is rejected. The auth
  file is never loaded implicitly by `cai_client_open`; callers must explicitly
  open auth and pass `config.chatgpt_auth`.
- Interactive ChatGPT login is exposed as a server-agnostic OAuth callback
  handler, not as a built-in webserver. Call `cai_chatgpt_login_start` with an
  auth file path or the library default and a redirect URI, open the returned
  authorization URL yourself or with `cai_chatgpt_login_open_browser`,
  pass the embedding server's callback request target into
  `cai_chatgpt_login_handle_callback`, then write the returned status,
  content-type, and body. The example `cai_example_chatgpt_login` shows this
  with a tiny local test server. The browser helper uses `open` on Darwin and
  `xdg-open` elsewhere by default, accepts a configured opener command, and
  never invokes a shell. Lua exposes the same flow with `cai.chatgpt_login`,
  `login:handle_callback`, `cai.chatgpt_login_open_browser`, and
  `cai.chatgpt_auth_default_path`; the Lua example uses LuaSocket only for its
  example listener.
- `CAI_DEFAULT_DOTENV_PATH` is `.env` for callers that explicitly want cai's
  dotenv parser. Call `cai_load_dotenv_api_key(path, env_name, &key, &error)`,
  pass the returned key to `config.api_key`, then release it with
  `cai_string_destroy` after `cai_client_open` has copied it.
  Dotenv contents are treated as untrusted input: cai does not execute or
  expand dotenv values, and rejects invalid variable names, overlong lines,
  unterminated quoted values, empty keys, and control characters in key values.
- OpenRouter can be selected with `cai_client_config_use_openrouter()`, which
  uses `OPENROUTER_API_KEY` and `https://openrouter.ai/api/v1`.
- Public config structs are zero-defaultable. A memset-zero config means "use
  cai defaults" for omitted knobs; fields are named so nonzero values opt into
  behavior or override a limit/path.
- Session continuity defaults to OpenAI-style server-side continuation.
  Client-side history replay is available for stateless compatible providers
  such as OpenRouter.
- The low-level Responses params surface exposes current non-WebSocket
  request controls used by modern Responses workflows: background mode, store,
  service tier, truncation, metadata, include, prompt-template JSON, text
  verbosity, reasoning, structured text format, server-side compaction,
  local function tools, raw hosted tool objects, raw JSON `tool_choice`, and
  `max_tool_calls`. `cai_client_count_response_input_tokens` wraps
  `/responses/input_tokens` and sends a counting-safe request clone, because
  the live endpoint accepts input-shaping fields such as tools and
  `tool_choice` but rejects generation/execution limits such as
  `max_output_tokens` and `max_tool_calls`.
- `lonejson` is the JSON layer and is linked as an external library, not
  compiled into cai. That keeps cai compatible with host applications such as
  Vectis that already provide their own lonejson instance.
- `pslog` is used as an external public header dependency for optional
  host-owned logging.
- Large inputs and outputs should stream through lonejson spooling/source/sink
  paths rather than being materialized in memory.

## Lua Binding

The Lua binding targets Lua 5.5 and is exposed as `require("cai")`. It wraps
the public C workflow facade: clients, agents, sessions, responses/outputs,
streaming sinks, tool registries, tool presets, ChatGPT auth/login helpers, and
the MCP Streamable HTTP handler.

Build and run the local LuaRock test:

```sh
make lua-test
```

The LuaRock depends on the `lonejson` Lua rock and links against an installed
`libcai` discovered through `pkg-config cai`; the local test target installs
the current debug build into `build/luarocks/cai-prefix` first. `lonejson`
0.31.0 is not assumed to exist on LuaRocks.org: `make lua-rock` installs it
from the official release source rock at
`https://github.com/sa6mwa/lonejson/releases/download/v0.31.0/lonejson-0.31.0-1.src.rock`
when needed. Lua projects can use the rock as a facade over the C library,
while Vectis can still call the C API directly where that fits its performance
and integration needs better.

For manual LuaRock installation, install the matching lonejson source rock
first because this version is served from the GitHub release, not LuaRocks.org:

```sh
luarocks install https://github.com/sa6mwa/lonejson/releases/download/v0.31.0/lonejson-0.31.0-1.src.rock
luarocks install cai-<version>-1.src.rock
```

Lua APIs that accept large text or file payloads have `*_spooled` variants.
Those accept strings, chunk-producing callbacks, or lonejson-style spooled
readers with `rewind()` and `read(n)`. Lua raw spooled tool callbacks receive
that same reader shape for arguments, and may return a chunk-producing callback
or spooled reader to stream JSON output without building the full value first.

The Lua facade intentionally does not expose C-only embedding surfaces such as
custom allocators, `FILE *`, `lc_source` / `lc_sink`, raw `cai_source` /
`cai_sink` constructors, lonejson C maps, or custom todo storage callbacks.
Lua code uses Lua callbacks/readers/writers for those integration points.

The Lua examples include a basic streaming agent, a terminal chatbot with
SearXNG and todo/kanban tools, streamed tool output, low-level conversation
handling, and session state save/restore. See
[examples/README.md](examples/README.md).
- Non-streamed JSON responses are capped by `json_response_limit_bytes`
  (default 1 MiB). Tool output can spill to disk with
  `tool_output_memory_limit` and can be hard-capped per auto-run with
  `tool_output_max_bytes`.
- Clients and sessions can enforce cumulative token and estimated USD spend
  budgets with `cai_usage_limits`. Zero leaves a limit unset. Client limits are
  shared across all agents/sessions on that client; session limits are per
  session and can be set directly or inherited from
  `cai_agent_config.session_usage_limits`. If `max_spend_usd` is positive, the
  selected model must have CAI pricing metadata; verified free OpenRouter models
  are accepted as zero-cost, but unknown or incomplete model metadata fails
  closed instead of silently estimating `$0.00`. CAI estimates spend with
  bundled Standard API token prices and applies known long-context token prices
  for models whose pricing changes above a documented input-token threshold. A
  request that crosses a budget is charged, returns `CAI_ERR_LIMIT`, and later
  requests are rejected before transport until the limit is raised or disabled.
  Use `cai_client_usage`,
  `cai_session_usage`, or `cai_session_close_with_usage` to report cumulative
  token usage and estimated USD spend:

  ```c
  cai_usage_limits limits;
  cai_usage_accounting spent;

  cai_usage_limits_init(&limits);
  limits.max_total_tokens = 100000;
  limits.max_spend_usd = 5.00;
  config.usage_limits = limits;

  /* ... run one or more sessions ... */
  cai_session_usage(session, &spent, &error);
  ```

  Lua exposes the same surface with `usage_limits` on `cai.open`,
  `session_usage_limits` on `client:new_agent`, and `usage()` /
  `set_usage_limits()` methods on clients and sessions.
- The C and Lua terminal-chat examples set a default session
  `max_output_tokens` budget of `1,000,000` cumulative output tokens. Override
  any budget lane with `--max-input-tokens`, `--max-cached-input-tokens`,
  `--max-output-tokens`, `--max-reasoning-output-tokens`,
  `--max-total-tokens`, or `--max-spend-usd`; pass `0` to disable that lane.
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
  `poolside/laguna-m.1:free`, because it passes cai's generic Responses
  compatibility and client-side continuity integration regressions.
- OpenRouter tool-calling integration uses
  `CAI_OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE` by default. The
  `openrouter/free` router advertises feature filtering for tool calls, but in
  practice it may route to a free model that reasons about calling the tool
  without emitting a function call.

## Consuming cai

Installed cai archives include both CMake package metadata and pkg-config
metadata. They are meant to keep consumer build files small and avoid manual
include/library path plumbing.

With pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs cai)
```

With CMake:

```cmake
find_package(cai CONFIG REQUIRED)
add_executable(app app.c)
target_link_libraries(app PRIVATE cai::cai_shared)
```

The metadata records the dependency mode used to build cai. In the default
`cpkt` mode it points at the matching official `c.pkt.systems` release URL and
checksum for the native curl/OpenSSL stack and the matching official lonejson
release URL/checksum. The CMake package models lonejson as an external
dependency: `find_package(cai)` requires `liblonejson` to be discoverable and
adds lonejson headers when they are available, but cai release archives do not
vendor lonejson or curl runtime libraries. The discovered host `liblonejson`
must also expose cai's expected ABI generation. Shared-only consumers that only
use `<cai/cai.h>` can configure without lonejson development headers; consumers
that include lonejson-backed cai headers or use static linking still need the
lonejson development package. The pkg-config file exposes lonejson as a public
requirement and libcurl as a private link requirement. `pslog.h`/`libpslog` is
only needed by source builds, logging integrations that construct pslog
loggers directly, and the MCP server examples. cai release archives do not
compile in single-header dependency variants.

## Logging

`cai_client_config.logger` accepts a borrowed `pslog_logger *`. cai never owns
or destroys that logger; set `logger_disabled` to nonzero to suppress logging
even when a logger is present.

Current cai-owned log events cover client setup, OpenRouter server-continuity
warnings, HTTP request start/completion, API error status responses, transport
failures, and configured HTTP response-size limit failures. API keys and
request/response bodies are not logged. HTTP request starts are trace-level,
successful completions are debug-level, 4xx API responses are warn-level, and
5xx/transport failures are error-level.

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

Large user text can use `add_user_text_source` or `add_user_text_spooled`
instead of building a temporary C string. Source inputs are consumed when added
and stored as spooled pending input; request retries then clone that spool, so
retry behavior is the same as for small string input.

User file input is available at the same facade level. Use
`add_user_file_path` for a local path, `add_user_file_source` for a `cai_source`,
or `add_user_file_data_spooled` when the caller already owns a
`lonejson_spooled` value. Source/path file inputs are consumed when added and
stored as spooled pending input; request retries then clone that spool, so a
failed request does not consume the queued file input.

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
For non-text output items such as hosted-tool, image, code, or future item
types, use `cai_response_output_items_json` for a materialized JSON array or
`cai_response_write_output_items_json` to stream the preserved output item array
into a sink. The per-item accessors expose common metadata fields, while the
preserved JSON is the compatibility path for item variants cai does not type
yet.

Tools are agent capabilities. The default `register_tool` path is a typed
lonejson callback; `register_raw_tool` is the full-string JSON escape hatch;
`register_raw_spooled_tool` is the raw JSON path for large arguments. The typed
path uses a lonejson map for parameters and a second lonejson map for the result.
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

OpenAI-hosted tools are different from local callback tools: OpenAI executes
them, so cai should not register a local callback for them. Use
`cai_response_create_params_add_simple_hosted_tool` or
`cai_response_create_params_add_hosted_tool_json` on low-level Responses
params, or `agent->add_simple_hosted_tool` / `agent->add_hosted_tool_json` on
the facade. The raw JSON hosted-tool path is intentionally the primary surface
for `mcp`, `file_search`, `code_interpreter`, `computer_use_preview`,
`image_generation`, `tool_search`, and future hosted tools, because their
schemas evolve faster than a C SDK should hard-code. cai validates the supplied
tool JSON with lonejson and streams it into the request body as a JSON value.
For remote MCP hosted tools, `cai_hosted_mcp_tool_config` is only a request
helper for server identity and exposure policy. Omit `allowed_tools` to expose
all remote tools from that MCP server, or provide `allowed_tool_names` /
`allowed_tools_json` to expose only named tools. `require_approval_json`,
`headers_json`, and the raw hosted-tool JSON path remain pass-through so cai
does not mirror or freeze remote tool schemas.

For large typed tool results, callbacks can put source-backed or spooled fields
into their result struct instead of building a raw JSON string. Use
`cai_tool_result_set_source_path` for lonejson source fields and
`cai_tool_result_set_spooled` for lonejson spooled string/base64 fields; cai
streams the resulting JSON into the tool output sink and then cleans up those
handles.

`cai_run_options.tool_output_max_bytes` is a total output cap for tool
auto-runs. Leave it at zero for unlimited output, or set it when tool output is
controlled by remote model decisions and should fail closed above a known size.

`cai_tool_schema_from_map` can be used when callers want to inspect or enrich
the generated schema. Metadata helpers such as `describe` update existing
properties; they do not decide requiredness. Requiredness belongs in the
lonejson field map so the C decoder and OpenAI schema cannot drift.

```c
cai_tool_schema_from_map(&lookup_customer_params_map, &schema, &error);
schema->describe(schema, "customer_id", "Customer id", &error);
```

Tool arguments and tool results are hostile boundaries. The typed tool path
validates argument JSON before callbacks run, rejects unknown fields against the
lonejson map, relies on lonejson for required/type/duplicate-key failures, and
serializes callback results as JSON values instead of raw text concatenation.
This prevents common prompt-injection payloads from smuggling extra fields into
typed callbacks or escaping JSON string boundaries on the way back to the
model. `register_raw_tool` deliberately bypasses the typed result API and
passes arguments as a full C string, so it is a convenience/materialized escape
hatch. Use `register_raw_spooled_tool` or
`cai_tool_registry_register_raw_spooled` when raw JSON arguments may be large;
cai validates the spooled value as JSON and passes the spool through without
rebuilding a whole argument string.

For streamed auto-run tool calls, `cai_tool_event.arguments_json` remains a
compatibility string for logging and simple handlers, while
`arguments_json_spooled` exposes the same validated arguments as a
`lonejson_spooled` handle. cai uses that spooled path for tool execution when
function-call arguments arrive through the streaming capture path.

## MCP Handler

`<cai/mcp.h>` exposes a transport-neutral MCP Streamable HTTP route handler for
serving a `cai_tool_registry` through MCP without making cai an HTTP server.
The host framework, for example Vectis/Kore, owns routing, TLS, auth,
timeouts, and connection lifecycle. cai receives a streaming request body,
reads logical headers through a callback, writes logical response headers
through a callback, and streams the JSON-RPC response into a caller-provided
sink.

First implemented scope:

- POST JSON-RPC requests with JSON replies by default.
- POST JSON-RPC requests can also reply as a single SSE event when the client
  asks for `Accept: text/event-stream` without `application/json`.
- GET `/mcp` returns an SSE stream endpoint for server-to-client transport
  compatibility. cai currently emits a lightweight SSE comment heartbeat and
  closes; server-initiated JSON-RPC notifications/requests are not implemented
  yet.
- `initialize`
- `notifications/initialized`
- `ping`
- `tools/list`
- `tools/call`
- optional stateful `Mcp-Session-Id` lifecycle through host-owned callbacks

The request body is a `cai_source` and the response body is a `cai_sink`.
The JSON-RPC envelope is parsed with lonejson while `id`, `params`, and
`params.arguments` are validated into spooled JSON values. Typed tool
arguments are parsed from that spool. Successful `tools/call` results are
bounded by `tool_output_max_bytes`, defaulting to
`CAI_MCP_DEFAULT_TOOL_OUTPUT_MAX_BYTES`, and cai intentionally uses bounded
spooling for that tool-call output so it can fail closed before committing a
partial JSON-RPC response. Set `tool_output_max_bytes` to
`CAI_MCP_TOOL_OUTPUT_UNLIMITED` only when the embedding route deliberately
accepts direct unbounded tool-output streaming. `GET`/SSE transport is
implemented for compatibility, but cai does not yet provide a host callback
surface for queued server-initiated MCP notifications or requests. Resources,
prompts, and sampling are not implemented yet.

MCP session persistence is storage-neutral. The zero/default MCP config is
stateless. Set `config.enable_sessions = 1` and provide
`cai_mcp_session_callbacks` to let the host create, load, save, and destroy
protocol sessions. cai returns `Mcp-Session-Id` on `initialize`, loads and
saves session state for later requests carrying that header, marks the session
initialized on `notifications/initialized`, and calls `destroy` for `DELETE`
requests with a session id. Tool state remains tool-owned; MCP session state
only tracks protocol lifecycle and client metadata.

Minimal route adapter shape:

```c
cai_mcp_http_request req;
cai_mcp_http_response res;

req.method = "POST";
req.body = request_body_source;
req.header = my_header_get;
req.header_context = route;

res.body = response_body_sink;
res.set_header = my_header_set;
res.header_context = route;

cai_mcp_handler_handle_http(handler, &req, &res, &error);
```

The handler asks for headers by lowercase logical names such as
`content-type`, `accept`, `origin`, `mcp-protocol-version`, and
`mcp-session-id`; adapters should normalize however their HTTP stack stores
header names. cai sets response headers such as `content-type`,
`mcp-protocol-version`, `cache-control`, and `x-content-type-options`
through the supplied callback.

By default, requests without an `Origin` header are accepted and requests with
a non-empty `Origin` header are rejected unless the origin is present in
`cai_mcp_handler_config.allowed_origins`. Set `disable_origin_validation` only
when the embedding server handles equivalent browser-origin protection itself.

The test build also provides `cai_mcp_http_server`, a tiny plain HTTP utility
for serving the MCP handler on `/mcp`. It is test/example infrastructure only
and is not linked into `libcai`. It binds `127.0.0.1`, defaults to port
`18765` for manual runs, and supports `--port 0 --print-port` for conflict-free
tests. Headers are parsed in memory, while request bodies remain socket-backed
`cai_source` streams pulled by the MCP handler.

An opt-in MCP Inspector e2e test validates the same server through the official
Inspector container image:

```sh
CAI_MCP_INSPECTOR_E2E=1 ctest --preset debug -R cai_mcp_inspector_e2e --output-on-failure
```

The test uses `ghcr.io/modelcontextprotocol/inspector:latest` through
`nerdctl run` or `docker run` and covers `tools/list` and `tools/call` over
Streamable HTTP. It is skipped by default so normal local test runs do not
depend on container image availability. Override the image with
`CAI_MCP_INSPECTOR_IMAGE`.

The example MCP server can also be attached to an agent client for manual
end-to-end testing of the production tool presets. Start it with an isolated
todo store:

```sh
cmake --build --preset debug --target cai_example_mcp_server
tmpdir=$(mktemp -d)
CAI_MCP_EXAMPLE_TODO_STORE="$tmpdir/todo.json" \
CAI_MCP_EXAMPLE_TODO_LOCK="$tmpdir/todo.lock" \
./build/debug/cai_example_mcp_server --port 18766
```

Then add the Streamable HTTP endpoint to Codex or Claude Code:

```sh
codex mcp add caiTodo --url http://127.0.0.1:18766/mcp
claude mcp add --transport http caiTodo http://127.0.0.1:18766/mcp
```

See [examples/mcp-server/README.md](examples/mcp-server/README.md) for the
equivalent config-file snippets and a todo-kanban workflow prompt that checks
tool discovery, help text, WIP-limit denial, completion, and board state.

Security fuzzing is available as an opt-in Clang/libFuzzer build:

```sh
make fuzz
make fuzz-smoke
make fuzz-full
```

Dedicated fuzzers cover the most exposed cai surfaces:

- `cai_tool_fuzz`: typed/raw tool argument handling plus hostile
  `exec_command`, `list_files`, and `read_file` inputs
- `cai_stream_fuzz`: Responses SSE framing, chunk-boundary handling, tool-call
  stream events, and completion metadata
- `cai_response_fuzz`: response/conversation/item parsing plus request JSON
  serialization
- `cai_mcp_fuzz`: MCP JSON-RPC request parsing, session persistence, JSON vs
  SSE replies, and tool dispatch
- `cai_session_fuzz`: large local-history/state export/import paths with spill
  to disk
- `cai_todo_fuzz`: todo/kanban rewrite, persistence, and structured operation
  churn

The fuzz build also registers one-iteration smoke tests in CTest so every
harness is built and executed in the standard gate for the `fuzz` preset.
Checked-in corpora under `tests/fuzz-corpus/` seed the harnesses with realistic
Responses SSE transcripts, response JSON, MCP JSON-RPC envelopes, session
history/state documents, and todo tool operations. `make fuzz-full` replays
those corpora through every harness for `CAI_FUZZ_RUNS` iterations per target
(`10000` by default).

Streaming callers that need function-call arguments can attach callbacks to
`cai_stream_sinks.function_call_arguments_delta` and
`cai_stream_sinks.function_call_arguments_done`. Callers that need raw streamed
output-item events for hosted tools, image/code outputs, and future item types
can attach `cai_stream_sinks.output_item_done`; cai passes common metadata plus
the raw item JSON as a `lonejson_spooled` value. Callers that need raw streamed assistant text
deltas before any terminal/UI affixes can attach
`cai_stream_sinks.output_text_delta`. `stream_text` remains text-only; the
callback surface is for callers that want to observe streamed tool-call
argument deltas and output items directly and decide their own orchestration
policy.

The ASAN preset is part of the local quality gate:

```sh
cmake --build --preset asan --target cai_tests
ctest --preset asan --output-on-failure
```

ThreadSanitizer and MemorySanitizer presets are also available. TSan runs the
local suite minus packaging checks that intentionally link non-instrumented
consumer smoke programs. MSan currently runs a curated smoke subset because
the normal cai dependency flow links against upstream binary dependencies; full
suite MSan is not meaningful across those uninstrumented library boundaries:

```sh
cmake --build --preset tsan --target cai_tests
ctest --preset tsan --output-on-failure
cmake --build --preset msan --target cai_tests
ctest --preset msan --output-on-failure
```

One historical ASAN failure looked like a lonejson crash in
`lonejson__owned_malloc`, but the root cause was cai's custom zeroing allocator
returning storage that was only aligned after a `size_t` header. lonejson wraps
custom allocator results in its own owned-allocation header, which needs
maximal C object alignment. cai's allocator header is now explicitly
over-aligned with pointer, integer, floating, and `long double` members. See
`stash/lonejson-custom-allocator-alignment-cr.md` for the matching lonejson
contract/documentation request.

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
- Metadata rows expose flags for verified, incomplete, inferred, deprecated,
  and provider-specific data.
- Runtime `/v1/models/{model}` checks may be used for availability, but not for
  context windows or feature discovery because the API does not return those
  facts.
- Auto-compaction must require known context metadata or an explicit caller
  token threshold. It should not guess.

The repeatable maintenance process for this curated table is documented in
[docs/model-metadata.md](docs/model-metadata.md).

In practice this means `cai` will likely need a reviewed generated metadata file
built from official OpenAI documentation/model comparison data. That is
unfortunate, but it is the only way to make default server-side
auto-compaction, context usage reporting, and feature validation useful until
OpenAI ships a proper model registry.

Relevant OpenAI documentation:

- <https://platform.openai.com/docs/api-reference/models/list>
- <https://platform.openai.com/docs/models/compare>
- <https://platform.openai.com/docs/api-reference/responses>

## Local SearXNG

cai includes a local SearXNG compose service for developing and testing the
web-search tool preset. The Makefile prefers `nerdctl compose` and falls
back to `docker compose` when nerdctl is not installed.

```sh
make searxng-up
make searxng-wait
make searxng-test
make searxng-down
```

The service listens on `http://127.0.0.1:8888` by default. Override the host
port with `CAI_SEARXNG_PORT` and query a different running instance with
`CAI_SEARXNG_BASE_URL`. The local config enables SearXNG JSON output so cai can
consume `/search?format=json`.

Automated smoke tests intentionally use an explicit upstream engine instead of
SearXNG's default fanout. The default `make searxng-test` engine is
`wikipedia`, which keeps the local test away from commercial search UI scraping
paths such as Google/Startpage. Manual local experimentation can still override
the engine:

```sh
CAI_SEARXNG_TEST_ENGINE=brave make searxng-test
```

Future cai web-search tool tests should keep this rule: use explicit engines,
and prefer documented API-backed engines such as SearXNG's `braveapi`,
Mojeek, or Marginalia when credentials are configured.

Agents or registries can register the opt-in SearXNG search preset by including
`<cai/tools/searxng.h>` and calling `cai_agent_register_searxng_tool` or
`cai_tool_registry_register_searxng_tool`. The
preset registers a typed lonejson tool named `searxng_search` by default. Its
input is `query`; its output is a compact typed result containing `query`,
`engine`, `title`, `url`, `snippet`, `source`, `result_count`, and
`infobox_count`. The callback fetches SearXNG JSON into bounded/spillable
`lonejson_spooled` storage, then streams the `results` and `infoboxes` arrays
with lonejson mapped-array streams. It counts every returned item but keeps only
the first usable result or infobox in the compact tool output. When
`config.engine` is unset, cai does not send `engines=...`; the SearXNG instance
uses its configured defaults.

Agents or registries can register the reverse-geocoding preset by including
`<cai/tools/revgeo.h>` and calling `cai_agent_register_revgeo_tool` or
`cai_tool_registry_register_revgeo_tool`. The default tool name is
`reverse_geocode`. Its required input is `latitude` and `longitude`; its typed
output contains `provider`, `label`, `city`, `municipality`, `region`,
`country`, `country_code`, `latitude`, and `longitude`. The default provider is
Nominatim at `https://nominatim.openstreetmap.org/reverse`, with a configurable
base URL, path, language, zoom, user agent, timeout, and bounded/spillable
response storage. Public Nominatim usage requires polite request volume and a
descriptive user agent; production deployments that need higher volume should
configure their own provider endpoint.

Agents or registries can register the persisted todo/kanban preset by
including `<cai/tools/todo.h>` and calling `cai_agent_register_todo_tool` or
`cai_tool_registry_register_todo_tool`. The default tool name is
`todo_kanban`. It exposes a single domain tool with an `operation` argument:
`help`, `create_board`, `list_boards`, `set_wip_limit`, `add_item`,
`list_board`, `current_work`, `move_item`, and `complete_item`. The registered
tool description and JSON schema include agent-facing usage guidance, operation
enums, field descriptions, and WIP-limit semantics. Agents can call
`operation=help` when they need an in-band reminder of the workflow.
When `board_id`, `board_key`, and `board_name` are omitted, the tool uses the
configured default board and lazily creates it if needed. The default board key
is `DEF`; other board keys are derived from board names unless `create_board`
is called with an explicit unique `board_key`. Calling `create_board` for an
existing board with a new unique `board_key` updates that board key. Item
sequence numbers are never caller-settable. `add_item` allocates a readable
public item reference from the board key and next sequence, for example
`DEF-001`, `DEF-087`, or `OPS-194`, while cai keeps its opaque internal ID in
storage. Tool input is lenient for item references: `DEF-001`, `DEF#1`,
`DEF001`, and `DEF1` all resolve to the same item, and results still return
the canonical dashed form. `list_boards` returns board records in a `boards`
array with `board_count`; item-listing operations return work records in an
`items` array with `item_count`.

By default the store is `$XDG_CONFIG_HOME/cai/todo.json`, falling back to
`$HOME/.config/cai/todo.json`, with `todo.lock` as the transaction lockfile.
Callers can override both paths in `cai_todo_tool_config`. Missing parent
directories and files are created at registration and operation time.
For non-file backends, set `cai_todo_tool_config.store` and
`store_context`. The callback store API is transaction-oriented: cai opens a
transaction, streams the current JSON document through a `lonejson_reader_fn`,
streams each rewritten document through a `lonejson_sink_fn`, calls
`commit_write` to promote each rewrite pass inside the transaction, then calls
`commit` once to publish the final document. `rollback` must discard staged
writes. This is the intended boundary for lockd-backed or application-owned
todo storage.

The todo store is one canonical JSON document:
`{ "version": 1, "boards": [], "items": [], "done": [] }`. cai reads selected
arrays one item at a time with lonejson array cursors and mutates selected
arrays with `lonejson_array_rewrite_*` under an advisory `fcntl` lock plus
temp-file/rename transaction boundary in the default file store. The complete
document and selected arrays are not materialized for storage operations. Tool
results are bounded by `max_result_items`; when a result is truncated it
includes `truncated=true`.
Moving an item into `in_process` respects the board's WIP limit and reports
`ok=false` with `code="wip_limit_exceeded"` as a normal structured tool result,
not as a transport failure.

Agents or registries can register the opt-in command execution preset by
including `<cai/tools/exec.h>` and calling `cai_agent_register_exec_tool` or
`cai_tool_registry_register_exec_tool`. The default tool name is
`exec_command`, intentionally matching the Codex-compatible model-facing
shape. Its required input is `cmd`; optional inputs are `workdir`, `shell`,
`tty`, `login`, `timeout_ms`, and `max_output_tokens`. This preset returns
after the process exits. `max_output_tokens` is treated as a per-call output
budget cap against the byte-oriented capture limit configured in
`cai_exec_tool_config`. When `tty=true` is allowed, cai uses the PTY only for
stdout/stderr capture; child stdin remains `/dev/null` and is not a terminal.

Command execution is never registered by default. Callers must provide a
`root_path`; every requested `workdir` is resolved under that root before
execution. On Linux the sandbox backend is `bwrap`; when `bwrap` is not
available, the tool fails closed. On Darwin/macOS, cai uses experimental
`sandbox-exec` support with a generated seatbelt profile that allows command
execution, system binary/library reads, file access under `root_path`, and
optional network access. Darwin support is intentionally weaker than the Linux
`bwrap` path and should be treated as experimental until validated on the
target macOS release. There is no non-sandboxed execution fallback.
The bwrap profile keeps the caller UID/GID, clears the inherited environment,
sets only `PATH`, `HOME`, `TMPDIR`, and `LANG`, isolates `/tmp` and
`/var/tmp`, unshares PID/IPC/UTS/network namespaces by default, and starts a
new session while binding only the configured root writable plus read-only
system paths needed to run normal commands. Set `allow_network` only when the
tool should have network access; in that mode cai also binds common resolver
and trust-store files such as `/etc/resolv.conf`, `/etc/hosts`, `/etc/ssl`,
and `/etc/pki` when present.
Linux cgroup v2 pids/memory limits are available with
`enable_cgroup_limits`; zero `pids_max` and `memory_max_bytes` leave those
controllers unset. If both are zero, cai skips cgroup setup entirely. If
cgroup setup cannot be applied, the tool fails closed
instead of running unbounded. `cgroup_parent_path` can point at a host-owned
writable cgroup subtree; otherwise cai uses `/sys/fs/cgroup`.
Output is captured through bounded `lonejson_spooled` fields and serialized as
structured JSON with `stdout`, `stderr`, combined `output`, exit/signal
metadata, timeout state, per-stream and combined-output truncation flags,
effective cwd, and the sandbox backend used. `output_max_bytes` is a total
retained-byte budget across those three text fields; cai prioritizes combined
`output` when the budget is tight, so `stdout` and `stderr` may truncate
earlier than `output`.

Agents or registries can register the opt-in file inspection presets by
including `<cai/tools/read.h>`. `cai_agent_register_read_tool` or
`cai_tool_registry_register_read_tool` registers `read_file`; its required
input is `path`, with optional `start_line`, `end_line`, and `max_bytes`.
`cai_agent_register_list_files_tool` or
`cai_tool_registry_register_list_files_tool` registers `list_files`; its inputs
are optional `path`, `recursive`, `include_hidden`, and `max_entries`.
Both tools require a `root_path`. Relative paths are resolved from
`default_workdir` or the root, absolute paths must already be inside the root,
and symlink/path traversal escapes are rejected after canonicalization.
`read_file` also rejects regular files with multiple hard links so a writable
root cannot smuggle in host files through a hard-link alias.
`list_files` returns bounded file metadata entries and does not follow symlink
directories. Regular files include `text_candidate` and `binary_candidate`
hints from a bounded prefix scan so an agent can avoid reading obvious binary
or control-character payloads. `read_file` rejects directories and streams file
content into a bounded `lonejson_spooled` result field with `path`,
`resolved_path`, line range, `byte_count`, `file_size`, and `truncated`
metadata. Prefer these tools over `exec_command` when an agent only needs to
inspect files. `read_file` is text-only: NUL bytes, invalid UTF-8, and control
characters other than tab/newline/carriage return/form feed are rejected instead
of being serialized back through the model context.

## Integration Tests

The default test suite is offline. Integration tests intentionally spend API tokens and
must be run explicitly by configuring the integration build. Once integration
tests are enabled, CTest enumerates the real API e2e cases; the environment
variables below are internal selectors for running one scenario directly, not
opt-in gates for the integration suite.

```sh
cmake --preset debug -DCAI_BUILD_INTEGRATION_TESTS=ON
cmake --build --preset debug
ctest --test-dir build/debug -L integration --output-on-failure

CAI_INTEGRATION_TODO_WORKFLOW=1 build/integration/cai_integration_tests
build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_DOTENV=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_SESSION=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_STREAM_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_STREAM_HISTORY=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_TOOL_SECURITY=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_READ_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_HOSTED_WEB_SEARCH=1 build/integration/cai_integration_tests
CAI_INTEGRATION_SEARXNG_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_TOOL_SECURITY=1 build/integration/cai_integration_tests
CAI_INTEGRATION_EXEC_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_READ_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_STATE_RESTORE=1 build/integration/cai_integration_tests
CAI_LUA_HOSTED_WEB_SEARCH_E2E=1 ctest --preset integration -R cai_lua_hosted_web_search_e2e --output-on-failure
CAI_LUA_TOOL_STREAM_E2E=1 ctest --preset integration -R cai_lua_tool_stream_e2e --output-on-failure
CAI_LUA_SESSION_E2E=1 ctest --preset integration -R cai_lua_session_continuity_e2e --output-on-failure
```

`CAI_INTEGRATION_OPENROUTER_DOTENV=1` clears any inherited
`OPENROUTER_API_KEY` process environment variable before opening the client,
then runs the OpenRouter Responses compatibility check. This verifies the cai
`.env` loader path with `.env` containing `OPENROUTER_API_KEY=...`.

`CAI_INTEGRATION_OPENROUTER=1` runs a basic Responses compatibility check
against OpenRouter using `OPENROUTER_API_KEY`, `https://openrouter.ai/api/v1`,
and `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` unless
`CAI_OPENROUTER_TEST_MODEL` overrides the model.

`CAI_INTEGRATION_OPENROUTER_SESSION=1` runs a two-turn OpenRouter session
continuity check using cai's client-side history replay mode. It does not test
OpenAI Conversations or server-side compaction on OpenRouter.

`CAI_INTEGRATION_OPENROUTER_TOOL=1` runs a typed lonejson tool-calling
regression against OpenRouter using client-side history replay and
`CAI_OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE` unless
`CAI_OPENROUTER_TOOL_TEST_MODEL` overrides the model. It verifies that the model
calls the registered tool, the callback result reaches the assistant answer,
and the next turn can recall that tool result through local history.

`CAI_INTEGRATION_OPENROUTER_STREAM_TOOL=1` runs the same style of OpenRouter
tool-calling regression through `cai_session_stream_auto`, validating streamed
tool execution, tool event callbacks, and client-side history replay after the
streaming tool turn.

`CAI_INTEGRATION_OPENROUTER_STREAM_HISTORY=1` runs a no-tool OpenRouter
streaming regression that verifies streamed assistant text is captured into
cai's client-side history and can be recalled on the next non-streaming turn.

`CAI_INTEGRATION_OPENROUTER_TOOL_SECURITY=1` runs the same hostile tool-output
regression as `CAI_INTEGRATION_TOOL_SECURITY=1`, but against OpenRouter with
client-side history replay and the known working free tool-call model.

`CAI_INTEGRATION_OPENROUTER_READ_TOOL=1` runs `list_files`/`read_file` against
OpenRouter through the real Responses-compatible API. It verifies list hints,
text reads, client-side continuation, and binary-file denial using the known
working free tool-call model.

`CAI_INTEGRATION_OPENROUTER_E2E=1` runs a multi-turn continuity eval against
OpenRouter using
`CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` and cai's client-side history replay
mode. The test paces requests by `CAI_OPENROUTER_E2E_DELAY_SEC`, defaulting to
4 seconds, because free OpenRouter models can have low per-minute request
limits. The OpenAI API-key E2E remains the longer 20-turn eval.

`CAI_INTEGRATION_SEARXNG_TOOL=1` runs a real OpenAI tool-calling regression
against a local SearXNG endpoint, defaulting to `http://127.0.0.1:8888` and the
explicit `wikipedia` engine. Start it with `make searxng-up` before running
the test, or set `CAI_SEARXNG_BASE_URL` to another SearXNG instance.

`CAI_INTEGRATION_HOSTED_WEB_SEARCH=1` runs a real OpenAI-hosted `web_search`
regression. It uses the generic hosted-tool JSON path, requires a hosted tool
call, validates structured raw JSON `tool_choice`, verifies
`max_tool_calls`, checks `/responses/input_tokens`, and fails unless the
response output items include `web_search_call`.
`CAI_LUA_HOSTED_WEB_SEARCH_E2E=1` runs the same hosted-tool path from Lua using
low-level `response_params` and fails unless the Lua response wrapper exposes a
`web_search_call` output item, `/responses/input_tokens` returns a positive
count, and token usage is present in the real API response.

`CAI_INTEGRATION_REVGEO_PROVIDER=1` runs the reverse-geocoding preset directly
against the default provider and asserts known Gothenburg coordinates resolve
to Sweden/SE with a recognizable Gothenburg label or city. This does not spend
OpenAI tokens, but it does call the public geocoding service.

`CAI_INTEGRATION_TOOL_SECURITY=1` runs a real OpenAI tool-output injection
regression. The registered tool returns text that looks like JSON role/system
messages and direct instructions to override the developer prompt. The test
passes only if the model treats that output as untrusted tool data and returns
the expected safe marker.

`CAI_INTEGRATION_EXEC_TOOL=1` runs a real OpenAI streaming tool regression for
the `exec_command` preset. The model must call the command tool against a
temporary sandbox root, run `ls`, `uname`, `cat`, `grep`, and `tar`, and then
attempt `cat /etc/passwd | head -n 1`. The test verifies tool events, final
assistant markers, `sandbox="bwrap"`, successful normal command output, and
that the host `/etc/passwd` escape is denied.

`CAI_INTEGRATION_READ_TOOL=1` runs a real OpenAI streaming tool regression for
the `list_files` and `read_file` presets. The model must inspect a sandboxed
test tree, read bounded UTF-8 text, avoid binary reads when list hints identify
a binary candidate, and observe a forced denial when explicitly asked to read a
binary file.

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
