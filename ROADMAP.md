# cai roadmap

This is the single active planning document for cai. It records the current
first-release baseline, explicit boundaries, and future work. Stable usage and
build documentation lives in [README.md](README.md).

## Current Prerelease Baseline

cai is a C89/POSIX SDK-style client for OpenAI-compatible Responses workflows.
The first prerelease target is the C SDK and examples, not Lua or WebSocket
transports.

Implemented:

- OpenAI Responses create/retrieve/delete/cancel style HTTP APIs used by the
  SDK.
- OpenAI Conversations endpoints and conversation-handle sessions.
- HTTP/SSE streaming with semantic event parsing.
- Agent/session facade with opaque handles, method-style function pointers,
  developer instructions, automatic server-side continuation, and default
  server-side compaction.
- Client-side history replay for stateless compatible providers such as
  OpenRouter.
- Text, image URL, file, source-backed, and spooled input paths where the
  public surface needs to handle large data.
- Tool registry with typed lonejson callbacks, raw JSON escape hatches,
  spooled raw arguments, source/spooled typed result fields, auto-run tool
  loops, and streamed tool-call argument capture.
- Model constants and curated metadata for current OpenAI/OpenRouter models
  used by the SDK and tests.
- External binary dependencies through `CAI_DEPENDENCY_MODE=cpkt`: official
  `c.pkt.systems`, `lonejson`, and `libpslog` release artifacts.
- Host dependency mode for already-installed curl/lonejson/pslog.
- libpslog-backed client logging.
- SearXNG, reverse-geocoding, and todo/kanban tool presets.
- Transport-neutral MCP Streamable HTTP handler for serving cai tool
  registries from host-owned HTTP servers.
- Test/example MCP HTTP servers. These are not linked into `libcai`.
- Example MCP server exposing reverse geocoding, todo/kanban, and Linux/X11
  clipboard when `xclip` is present.
- Terminal chat, SearXNG, SMHI weather, Mike Mind, session-state,
  history-export, conversation-handle, OpenRouter, streaming, and basic
  response examples.
- Release matrix packaging for Linux x86_64/aarch64/armhf with GNU and musl
  variants plus Darwin arm64 when osxcross is available.
- Source archive packaging.
- Release verification for archive roots, docs, pkg-config/CMake metadata,
  dependency exclusion, sanitizer exclusion, and host-free `$ORIGIN` runpaths.

Recent prerelease gates run successfully:

- Debug build and offline tests.
- ASan/UBSan build and tests.
- `cai_tool_fuzz -runs=10000`.
- Release matrix packaging and archive verification.
- Source archive smoke build.
- MCP Inspector container e2e.
- Local SearXNG smoke test.
- OpenAI 20-turn session e2e.
- OpenAI state-restore e2e.
- OpenAI hostile tool-output regression.
- OpenRouter basic/session/tool/stream-tool/stream-history/tool-security e2e.
- OpenRouter 20-turn client-history e2e with request pacing.
- Reverse-geocoding provider e2e.
- SearXNG-backed OpenAI tool e2e.

## Active First-Release Work

Before tagging the first C SDK prerelease:

- Keep README, examples, and installed docs aligned with the current API.
- Run the complete prerelease verification cycle on the release candidate tag.
- Verify `make release` outputs:
  - binary SDK archives as `dist/cai-<version>-<target>.tar.gz`,
  - source archive as `dist/cai-<version>.tar.gz`,
  - checksum file as `dist/cai-<version>-CHECKSUMS`.
- Verify archives contain only cai headers, `libcai`, CMake/pkg-config
  metadata, and docs. Dependency headers/libraries stay external.
- Verify release builds do not contain sanitizer artifacts, host paths, or
  non-relocatable runpaths.

## Parked: Lua Binding

Status: next major product feature after the C SDK prerelease.

Goal:

- Wrap the public C SDK surface, not internal HTTP/SSE transport details.
- Support client, agent, session, response, streaming sinks, and tool
  registration.
- Keep logger injection C-owned. Lua should not need to hop C/Lua boundaries
  for pslog.
- Package a LuaRock and source rock under `dist/`, following the release style
  used by the neighboring libraries.

Initial scope:

- Lua module under `lua/`.
- Lua-friendly error surface.
- Lua wrappers for:
  - client config/open/close,
  - agent config/new/destroy,
  - session send/stream/state save/restore,
  - basic response output/usage access,
  - registering Lua callbacks as cai tools.
- Lua tests against the offline mock paths first.
- Optional live integration examples only after the C surface remains stable.

Open design points:

- Exact Lua object model and naming.
- How much of `lonejson` should be exposed directly to Lua callers versus
  wrapped behind cai-specific helpers.
- LuaRock dependency declarations for cai/lonejson/libpslog binary artifacts.

## Parked: Responses WebSocket

Status: documented by OpenAI, not implemented.

OpenAI documents Responses WebSocket mode as a persistent connection to
`/v1/responses` for long-running, tool-call-heavy workflows. Each turn sends
new input items plus `previous_response_id`, so it is still Responses
state/continuation semantics, not Realtime.

Initial future scope:

- Choose a WebSocket transport compatible with the cai release matrix.
- Share semantic Responses event parsing with the existing HTTP/SSE path.
- Keep HTTP/SSE as the default transport and fallback.
- Add a repo-local C mock WebSocket server before live API-backed tests.
- Support text/reasoning deltas and function-call argument events first.
- Preserve true streaming: WebSocket frame buffers and lonejson chunk buffers
  are fine; full response/event materialization is not.
- Keep the public DX explicit while the feature settles, for example a response
  transport knob rather than silent transport switching.

Official OpenAI docs checked:

- <https://developers.openai.com/api/docs/guides/websocket-mode>
- <https://developers.openai.com/api/docs/guides/streaming-responses>

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

Current MCP support is intentionally a route-handler facade and tool-serving
subset. cai does not include Kore or any production HTTP server.

Implemented MCP methods:

- `initialize`
- `notifications/initialized`
- `ping`
- `tools/list`
- `tools/call`

Future MCP work:

- GET/SSE stream support if needed by host integrations.
- Stateful `Mcp-Session-Id` lifecycle.
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
  `poolside/laguna-xs.2:free`.
- OpenRouter Responses beta is stateless for cai's tested behavior, so
  multi-turn OpenRouter sessions use client-side history replay.
- Do not claim OpenRouter parity for OpenAI Conversations or server-side
  compaction until those behaviors are documented or separately proven.
- The long OpenRouter e2e is paced by `CAI_OPENROUTER_E2E_DELAY_SEC` because
  free models can have low per-minute request limits.

## Deferred Integrations

- Lockd-backed todo storage is not implemented in this repository. The
  todo/kanban preset already exposes a transaction-oriented callback storage
  interface so Vectis or lockd integration can live outside cai.
- SMHI weather remains an example-only tool. It is not a public cai tool preset.
- OpenAI organization billing/cost APIs are not used as a hard gate. cai uses
  local estimated spend limits from model pricing metadata for integration
  tests; billing telemetry can be revisited later.
