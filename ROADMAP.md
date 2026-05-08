# cai roadmap

This document tracks useful future features that are outside the current
implementation plan. Items here should not be treated as active milestones until
we deliberately promote them back into [PLAN.md](PLAN.md).

## Future: Realtime WebSocket

Status: future feature, not implemented.

Realtime WebSocket is officially documented by OpenAI and is a valid future cai
target, but it is not part of the current implementation work. It should remain
separate from normal Responses sessions because it has a different lifecycle,
event model, audio behavior, and low-latency use case.

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

Realtime is event-native, not Responses over WebSocket. The documented flow
includes:

- `session.created` after connect,
- `session.update` from the client,
- `conversation.item.create` for user text,
- `response.create` to start generation,
- `response.output_text.delta`,
- `response.output_text.done`,
- `response.done`,
- `error`.

Possible public API shape:

```c
typedef struct cai_realtime_session cai_realtime_session;
typedef struct cai_realtime_config cai_realtime_config;
typedef struct cai_realtime_event cai_realtime_event;

typedef int (*cai_realtime_event_fn)(void *context,
                                     const cai_realtime_event *event,
                                     cai_error *error);

typedef struct cai_realtime_handler {
  cai_realtime_event_fn on_event;
  void *context;
} cai_realtime_handler;

typedef struct cai_realtime_config {
  const char *model;                  /* default gpt-realtime */
  const char *instructions;           /* session.update instructions */
  const char *voice;                  /* ignored for text-only milestone */
  int text_only;                      /* default 1 in first milestone */
  int max_output_tokens;
  cai_tool_registry *tools;           /* borrowed */
} cai_realtime_config;

int cai_client_open_realtime(cai_client *client,
                             const cai_realtime_config *config,
                             cai_realtime_session **out,
                             cai_error *error);
int cai_realtime_session_update(cai_realtime_session *session,
                                const cai_realtime_config *config,
                                cai_error *error);
int cai_realtime_send_text(cai_realtime_session *session,
                           const char *text,
                           cai_error *error);
int cai_realtime_create_response(cai_realtime_session *session,
                                 cai_error *error);
int cai_realtime_run(cai_realtime_session *session,
                     const cai_realtime_handler *handler,
                     cai_error *error);
int cai_realtime_send_text_response(cai_realtime_session *session,
                                    const char *text,
                                    cai_sink *output_text,
                                    cai_error *error);
void cai_realtime_close(cai_realtime_session *session);
```

Implementation requirements before promotion:

- Choose and vendor/provision a WebSocket transport path compatible with the
  project build matrix.
- Add a C mock WebSocket server before adding live Realtime integration tests.
- Parse known Realtime events into typed fields and preserve full event JSON as
  an escape hatch.
- Surface unknown event types instead of treating them as fatal protocol
  errors.
- Reuse `cai_tool_registry` for synchronous local tools.
- Accumulate streamed function-call arguments into spooled storage and serialize
  function-call outputs through lonejson maps.
- Keep large text, function arguments, function outputs, and future audio chunks
  on spooled/source/sink paths.
- Add Realtime model metadata such as `CAI_MODEL_GPT_REALTIME` only when the
  feature is promoted.

Official OpenAI docs checked:

- <https://developers.openai.com/api/docs/guides/realtime-websocket>
- <https://developers.openai.com/api/docs/guides/realtime-conversations>
- <https://developers.openai.com/api/reference/resources/realtime>
- <https://developers.openai.com/api/docs/models/gpt-realtime>

## Future: Responses WebSocket

Status: documented future feature, not implemented.

OpenAI now documents Responses WebSocket mode as a persistent connection to
`/v1/responses` for long-running, tool-call-heavy workflows. Each turn sends
only new input items plus `previous_response_id`, so it is still Responses
state/continuation semantics, not the Realtime API.

Initial future scope:

- Choose a WebSocket transport compatible with the cai release matrix.
- Share semantic Responses event parsing with the existing HTTP/SSE path.
- Keep HTTP/SSE as the default transport and fallback.
- Add a C mock WebSocket server before live API-backed tests.
- Support text/reasoning deltas and function-call argument events first.
- Preserve true streaming: curl/WebSocket frame buffers and lonejson chunk
  buffers are fine; full response/event materialization is not.
- Keep the public DX explicit while the feature settles, for example a
  `cai_response_transport_websocket` or session config knob rather than silent
  transport switching.

Official OpenAI docs checked:

- <https://developers.openai.com/api/docs/guides/websocket-mode>
- <https://developers.openai.com/api/docs/guides/streaming-responses>
