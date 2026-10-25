# cai roadmap

This is the single active planning document for cai. It records the current
first-release baseline, explicit boundaries, and future work. Stable usage and
build documentation lives in [README.md](README.md).

## Current Prerelease Baseline

cai is a C89/POSIX SDK for Responses and OpenAI-compatible Chat Completions
workflows. The current products are core `libcai`, the embeddable `libcai_app`,
the static musl `cai` terminal agent, Lua 5.5 facades, and examples.
Responses WebSocket streaming supports OpenAI/ChatGPT; Realtime WebSocket
remains future work.

Implemented:

- OpenAI Responses create/retrieve/delete/cancel style HTTP APIs used by the
  SDK.
- OpenAI Conversations endpoints and conversation-handle sessions.
- HTTP/SSE streaming with semantic event parsing.
- Responses WebSocket streaming for OpenAI API-key and ChatGPT subscription
  auth clients, sharing the same semantic Responses event parser as HTTP/SSE.
- Agent/session facade with opaque handles, method-style function pointers,
  developer instructions, automatic server-side continuation, and default
  server-side compaction.
- Client-side history replay for stateless compatible providers such as
  OpenRouter.
- Chat Completions text/reasoning, image input, function/custom tools, refusal
  handling, usage accounting, and bounded compaction/continuation, including
  the Berget provider and endpoint-selected model catalog policy.
- Text, image URL, file, source-backed, and spooled input paths where the
  public surface needs to handle large data.
- Tool registry with typed lonejson callbacks, raw JSON escape hatches,
  spooled raw arguments, source/spooled typed result fields, auto-run tool
  loops, and streamed tool-call argument capture.
- Low-level Responses request controls for background mode, store, service
  tier, truncation, metadata, include, prompt-template JSON, text verbosity,
  structured text output, reasoning, server-side compaction, raw JSON
  `tool_choice`, `max_tool_calls`, `/responses/input_tokens`, and raw
  OpenAI-hosted tool objects.
- Agent facade support for OpenAI-hosted tools through validated raw tool JSON
  and simple `{ "type": ... }` hosted-tool helpers. Remote MCP hosted tools
  also have a generic config helper for server identity, allowed tool names or
  raw `allowed_tools` policy JSON, and raw approval/header JSON pass-through.
- Preserved response output-item JSON with sink streaming helpers, so callers
  can handle hosted-tool, image, code, and future output item variants even when
  cai only types common metadata fields.
- Streamed output-item done callbacks with raw item JSON for hosted-tool,
  image/code, and future streamed output variants.
- Model constants and curated metadata for current OpenAI/OpenRouter models
  used by the SDK and tests.
- External binary dependencies through `CAI_DEPENDENCY_MODE=cpkt`: official
  `cpkt`, `cpktmisc`, `lonejson`, and `libpslog` release artifacts. cpktmisc's
  `CpktYaml` supplies libyaml; there is no private YAML build.
- Host dependency mode for already-installed curl/lonejson/pslog and CpktYaml
  with its compatible cpkt core SDK.
- libpslog-backed client logging.
- SearXNG, reverse-geocoding, todo/kanban, and opt-in command execution tool
  presets. The command preset uses a Codex-compatible `exec_command` schema,
  requires an embedding-owned root path, captures output into bounded
  lonejson-spooled fields, hardens bwrap with isolated environment/temp
  namespaces, supports optional Linux cgroup v2 pids/memory limits, and fails
  closed when configured sandboxing cannot be applied.
- Transport-neutral MCP Streamable HTTP handler for serving cai tool
  registries from host-owned HTTP servers.
- Optional MCP `Mcp-Session-Id` lifecycle persistence through host-owned
  create/load/save/destroy callbacks.
- Test/example MCP HTTP servers. These are not linked into `libcai`.
- Example MCP server exposing reverse geocoding, todo/kanban, and Linux/X11
  clipboard when `xclip` is present.
- Terminal chat, SearXNG, SMHI weather, Mike Mind, session-state,
  history-export, conversation-handle, OpenRouter, streaming, and basic
  response examples.
- Lua 5.5 binding with client/agent/session/response/output handles, streaming
  sinks, streamed tool output callbacks, function-call argument stream
  callbacks, raw and raw-spooled Lua callback tools, lonejson-style spooled
  large-value inputs, public tool presets, hosted-tool helpers, ChatGPT
  auth/login helpers, MCP handler exposure, low-level Responses and
  Conversations handles, raw JSON `tool_choice`, input-token counting, model
  constants/metadata, offline tests, Lua examples, and local/release LuaRock
  build targets.
- Release matrix packaging for Linux x86_64/aarch64/armhf with GNU and musl
  variants plus Darwin arm64 when osxcross is available.
- Source archive packaging.
- Release verification for archive roots, docs, pkg-config/CMake metadata,
  dependency exclusion, sanitizer exclusion, and host-free relative runpaths.

Verification tiers:

- `make prerelease`
  - native Debug core/CLI, Valgrind, fuzz smoke, C/Lua facade and offline tests,
  - local MCP Everything end-to-end checks,
  - selected Release target builds, native Release tests and ARM QEMU smoke,
  - complete SDK/CLI/source/Lua packaging, checksums and extracted consumers.
  Host-only fixtures run in Debug and are omitted from the Release selection.
- `make prerelease-live`
  - credentialed integration tests, requiring `CAI_ENABLE_INTEGRATION_TESTS=1`.
  The same opt-in adds live tests to ordinary prerelease preparation.
- `make prerelease-hardening`
  - prerelease with live coverage, then longer fuzzing via `scripts/fuzz.sh long`.
  It requires the same live-test opt-in.
- `make format-check` checks formatting without configuring or building.
  `make asan` and `make test-coverage` are separate explicit gates.

Every committed implementation slice should be finalized with `make format`
before the commit is created. For ordinary local C changes, `make
finalize-slice` is the default shortcut: it runs `make format` and the debug
CTest suite. Broader changes should add the relevant Lua, sanitizer, fuzz,
integration, example, or release gates before committing.

Historical hardening runs have covered the checks below. These are examples
of coverage, not proof that the current tree passed a release gate:

- `cai_tool_fuzz -runs=10000`, covering typed/raw tools plus hostile
  `todo_kanban`, `exec_command`, `list_files`, and `read_file` inputs.
- Source archive smoke build.
- MCP Inspector container e2e.
- Local SearXNG smoke test.
- OpenAI 20-turn session e2e.
- OpenAI state-restore e2e.
- OpenAI hostile tool-output regression.
- Responses WebSocket offline tests for handshake/header validation, normal
  streaming, large frames, pre-stream transient retry, midstream disconnect
  failure, multi-turn server-side continuation, stale keepalive reconnect,
  response id propagation, and usage capture.
- Dedicated live OpenAI Responses WebSocket e2e that forces the WebSocket
  transport, alternates `stream`, `stream_text`, and `stream_auto`, exercises a
  streamed local tool call, validates streamed tool events/arguments, checks
  state continuity, and verifies per-turn token usage.
- OpenRouter basic/session/tool/stream-tool/stream-history/tool-security and
  read/list preset e2e.
- OpenRouter 20-turn client-history e2e with request pacing.
- Reverse-geocoding provider e2e.
- SearXNG-backed OpenAI tool e2e.
- Lua MCP todo/kanban facade e2e.
- Lua OpenAI streamed-tool e2e.
- Lua OpenAI streamed session-continuity e2e.
- ChatGPT subscription-auth 11-turn e2e covering Responses WebSocket streaming,
  streamed local tools, continuity, and token usage.
- OpenAI hosted `web_search` e2e covering raw hosted-tool JSON, structured
  `tool_choice`, `max_tool_calls`, and `/responses/input_tokens`.
- Lua OpenAI hosted `web_search` e2e covering the same hosted-tool and
  input-token-counting path.
- Full `make release` gate, including release CTest, binary/source archive
  packaging, Lua rock artifacts, checksums, and archive verification.

## Active First-Release Work

The `cai` CLI ships static musl binaries for Linux x86_64, aarch64 and armhf.
It wraps `libcai_app` and uses the Smith runtime, softline 0.12.0, libmdf 0.15.0,
ChatGPT or API-key providers, and encrypted liblockdc Pouch sessions/auth.
The same application SDK also ships static/shared libraries for all three
Linux architectures with glibc and musl. Core libcai's independent JSONL
backend remains available to SDK hosts.
Configured Streamable HTTP MCP servers use `[mcp.NAME]` sections; standalone
libcai MCP clients remain available to embedders.
Ship a `cai(1)` manual page in the binary archive alongside the executable,
README, and LICENSE using the SDK's relocatable install layout.
The CLI status bar reads ChatGPT subscription quota windows when the backend
provides them. Refresh runs in a background worker on startup and completed
turns, with a five-minute debounce and no idle polling; `/status` requests a
refresh explicitly.

Before tagging a release:

- Keep README, examples, and installed docs aligned with the current API.
- Keep public headers clangd-helpful: exported functions, structs, callbacks,
  method-table fields, and macros need concise documentation comments.
- Run incremental prerelease preparation on the development branch, optionally
  setting `CAI_VERSION_OVERRIDE` to the candidate version. The final release
  gate requires the signed default-branch commit and its exact lightweight tag,
  a clean tree and no candidate override.
- Verify `make release` outputs:
  - binary SDK archives as `dist/cai-<version>-<target>.tar.gz`,
  - source archive as `dist/cai-<version>.tar.gz`,
  - checksum file as `dist/cai-<version>-CHECKSUMS`.
- Verify SDK archives contain public cai headers, core libraries, application
  libraries on Linux, CMake/pkg-config metadata and docs. Separate CLI archives
  contain the static binary, manual and embedding examples. Dependency SDK
  headers/libraries stay external; required licenses accompany the products.
- Verify release builds do not contain sanitizer artifacts, host paths, or
  non-relocatable runpaths.

## Ongoing: ChatGPT Catalog Compatibility

Track upstream Codex client/protocol changes during provider maintenance and
before a release. The catalog compatibility baseline is maintained separately
from CAI's release version and conversation compaction hashes.

- Current baseline: `0.160.0` in `CAI_CODEX_CATALOG_CLIENT_VERSION`
  (`src/cai_model_catalog.c`). Last account-catalog verification: 2026-10-04;
  the account returned `gpt-6.1-sol` among 10 models. Availability and counts
  are account-specific evidence, not universal test expectations.
- Compare the current and proposed baseline against the same signed-in
  account, retaining CAI's own originator and User-Agent. Check returned model
  IDs, reasoning choices, context limits, and compatibility metadata.
- Verify the catalog adapter and Responses protocol before advancing the
  advertised baseline. Update its constant, request-URL regression fixtures,
  and maintenance documentation together.
- Preserve every returned ChatGPT model, including unfamiliar IDs. Keep
  regression coverage for visibility/API-support flags and catalogs exceeding
  50 entries.

The update and verification procedure is maintained in
[Model Metadata Maintenance](docs/model-metadata.md#codex-compatibility-baseline-maintenance).

## Follow-up: Plain-Link Terminal Wrapping

Track an upstream libmdf wrapping issue when OSC8 is disabled: at width 24 with
a left margin of 2, `abcdefghijklmnopqrst ([label](https://x.test)).` renders
the visible URL on a row without the left margin and inserts margin spaces
before the final period. A direct public-API reproduction produced identical
output with libmdf 0.13.0, 0.14.0 and 0.15.0, for whole and fragmented input.
Rechecked on 2026-10-19 with the public 0.15.0 API; the wrapping issue remains.
The 0.15.0 upgrade fixes parenthesized link destinations, which the CLI tests
cover in whole and fragmented deltas. OSC8 rendering is a separate path.

## Deferred: Embeddable application on Darwin

`libcai_app` ships static/shared SDK libraries for Linux x86_64, aarch64 and
armhf, each with glibc and musl. The standalone binary ships for all three
Linux architectures with musl.
Darwin arm64 application dependency packages are available, but application
notification/timer descriptors, PTY handoff, environment restoration, Chrome
host registration and Mach-O/Lua packaging need a deliberate port. Keep the
application disabled there until the public embedding contract and native
terminal behavior are verified. The concrete assessment is in
[the embedding guide](docs/embedding.md#darwin-port-assessment).

## Supported: Responses WebSocket

Status: implemented for OpenAI API-key and ChatGPT subscription-auth streaming
sessions.

OpenAI documents Responses WebSocket mode as a persistent connection to
`/v1/responses` for long-running, tool-call-heavy workflows. Each turn sends
new input items plus `previous_response_id`, so it is still Responses
state/continuation semantics, not Realtime. cai implements that transport with
libcurl WebSocket support, the documented OpenAI beta header, incremental
`response.create` messages, semantic event parsing shared with HTTP/SSE,
keepalive reuse, one transient pre-stream retry, stale-connection reconnect,
and one transparent ChatGPT OAuth refresh retry on 401/403.

The current public DX remains the normal cai streaming API: OpenAI API-key and
ChatGPT subscription-auth clients use Responses WebSocket for streaming, while
OpenRouter, local mock servers, and other compatible providers use HTTP/SSE.

Covered locally:

- C mock WebSocket server with handshake and OpenAI beta-header validation.
- Normal text streaming, response id propagation, and usage capture.
- Large WebSocket text frames.
- Transient upgrade failure followed by retry.
- Midstream disconnect after a partial text delta, which fails closed without a
  completed response id or usage.
- Multi-turn server-side continuation over a persistent connection.
- Stale keepalive reconnect between turns.

Covered against live providers:

- `CAI_INTEGRATION_RESPONSES_WEBSOCKET_E2E=1` forces the OpenAI Responses
  WebSocket path and verifies multi-turn continuity, streamed text, streamed
  local tool calls, tool arguments/events, and token usage.
- `CAI_INTEGRATION_CHATGPT_SUBSCRIPTION_E2E=1` exercises the ChatGPT
  subscription-auth WebSocket path across 11 turns with streamed tool calls and
  OAuth refresh behavior.
- Simulated disconnect/reconnect fault injection is intentionally covered by
  the mock WebSocket server rather than by trying to break the real OpenAI
  service connection.

Official OpenAI docs checked:

- <https://developers.openai.com/api/docs/guides/websocket-mode>
- <https://developers.openai.com/api/docs/guides/streaming-responses>

## Supported: Agent Conversation Export

Core libcai exposes `cai_agent_runtime_export_markdown` as a real incremental,
sink-based handover projection of the active runtime. It includes instructions,
conversation text, activity and goal metadata, without executing tools or
embedding binary attachments. It is not a resumable session format.
The optional file helper creates a new owner-only file without overwriting.

The application implements `/export [ID]` and `--export ID`, writing resumable
JSONL plus this Markdown projection under the application data directory's
`exports/ID/`. `--import FILE` imports JSONL into a new conversation.
Exports are explicit user/host actions, not a model tool. Desktop clipboard
integration remains host-owned. See [the handover contract](docs/agent-smith-spec.md#115-user-initiated-transcript-export)
and [CLI storage/export usage](README.md#cai-terminal-agent).

## Parked: Realtime WebSocket

Status: future feature, not implemented.

Realtime WebSocket is a separate OpenAI API surface for low-latency text/audio
sessions. It has a different lifecycle, event model, audio behavior, and use
case from normal Responses sessions.

Use cases:

- low-latency interactive sessions,
- server-side voice or speech-to-speech agents,
- backend WebSocket clients,
- live tool calls during an open session,
- later audio, SIP, WebRTC, and browser ephemeral-key flows.

Initial future scope:

- Connect to `wss://api.openai.com/v1/realtime?model=...`.
- Reuse cai client config principles: API-key resolution, `.env`, timeout,
  logger, allocator, and base URL handling where practical.
- Implement a separate `cai_realtime_session` facade.
- Support text input, text output, and synchronous local function tools first.
- Defer audio streaming, SIP, WebRTC, browser ephemeral-key flows, and MCP
  tools.

Realtime is event-native. The documented flow includes:

- `session.created` after connect,
- `session.update` from the client,
- `conversation.item.create` for user text,
- `response.create` to start generation,
- `response.output_text.delta`,
- `response.output_text.done`,
- `response.done`,
- `error`.

Implementation requirements before promotion:

- Choose and provision a WebSocket transport path compatible with the project
  build matrix.
- Add a repo-local C mock WebSocket server before adding live Realtime
  integration tests.
- Parse known Realtime events into typed fields and preserve full event JSON as
  an escape hatch.
- Surface unknown event types instead of treating them as fatal protocol
  errors.
- Reuse `cai_tool_registry` for synchronous local tools.
- Accumulate streamed function-call arguments into spooled storage and
  serialize function-call outputs through lonejson maps.
- Keep large text, function arguments, function outputs, and future audio chunks
  on spooled/source/sink paths.
- Add Realtime model metadata such as `CAI_MODEL_GPT_REALTIME` only when the
  feature is promoted.

Official OpenAI docs checked:

- <https://developers.openai.com/api/docs/guides/realtime-websocket>
- <https://developers.openai.com/api/docs/guides/realtime-conversations>
- <https://developers.openai.com/api/reference/resources/realtime>
- <https://developers.openai.com/api/docs/models/gpt-realtime>

## Future MCP Scope

The MCP server is a route-handler facade and tool-serving subset; cai does not
include Kore or any production HTTP server. Separately, the MCP client supports
tools, resources, prompts and completions, and libcai_app configures remote
Streamable HTTP tool servers. The deferred list below concerns server features.

Implemented MCP methods:

- `initialize`
- `notifications/initialized`
- `ping`
- `tools/list`
- `tools/call`
- Streamable HTTP GET/SSE heartbeat endpoint.
- SSE-only POST response mode for successful JSON-RPC replies.

Future MCP work:

- Host callback surface for queued server-initiated JSON-RPC notifications or
  requests on the GET/SSE stream.
- Resources.
- Prompts.
- Sampling.
- Additional conformance testing if the official MCP Inspector expands its
  Streamable HTTP coverage.

## Provider Boundaries

OpenAI is the primary target. OpenRouter is supported as a compatible Responses
provider where behavior is proven by integration tests.

Current OpenRouter boundary:

- `cai_client_config_use_openrouter()` selects `OPENROUTER_API_KEY` and
  `https://openrouter.ai/api/v1`.
- `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` is
  `openai/gpt-5.6-luna`.
- OpenRouter Responses beta is stateless for cai's tested behavior, so
  multi-turn OpenRouter sessions use client-side history replay.
- Do not claim OpenRouter parity for OpenAI Conversations or server-side
  compaction until those behaviors are documented or separately proven.
- The long OpenRouter e2e is paced by `CAI_OPENROUTER_E2E_DELAY_SEC` because
  free models can have low per-minute request limits.

## Integration Boundaries

- Shared-Pouch conversation messaging is implemented in libcai_app using
  managed LQ consumer services, stable workspace mailbox names, takeover fencing,
  durable delivery ACKs, and softline's
  normal approval queue. Core libcai offers generic tool-registration and
  next-turn hooks without depending on liblockdc. Remote lockd fanout remains
  deferred; see [messaging](docs/agent-messaging.md).
- Shared kanban is implemented: per-conversation selection, `/todo`, C/Lua
  receivers, and a read-only automatically updating browser handler. Board
  storage uses the application's encrypted `cai.todo` Pouch document. HTTP
  listeners, routing and access control remain host-owned; see
  [kanban integration](docs/kanban.md).
- SMHI weather remains an example-only tool. It is not a public cai tool preset.
- OpenAI organization billing/cost APIs are not used as a hard gate. cai uses
  local estimated spend limits from model pricing metadata for integration
  tests; billing telemetry can be revisited later.
