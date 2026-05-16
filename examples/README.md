# CAI Examples

These examples are built by default with `CAI_BUILD_EXAMPLES=ON`. They call the
real OpenAI API when run, so they require either `OPENAI_API_KEY` in the
environment or a repo-local `.env` file containing `OPENAI_API_KEY=...`.

The agent-oriented examples use the method-style handle facade (`client->...`,
`agent->...`, `session->...`). Raw Responses examples still use free functions
because they demonstrate request construction close to the wire API.

## Basic Response

```sh
cmake --build --preset debug --target cai_example_basic_response
./build/debug/cai_example_basic_response
```

## OpenRouter Response

Run the same Responses-style request through OpenRouter. This example uses
`OPENROUTER_API_KEY`, `https://openrouter.ai/api/v1`, and
`CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES`. Set `CAI_OPENROUTER_EXAMPLE_MODEL` to
override the model.

```sh
cmake --build --preset debug --target cai_example_openrouter_response
OPENROUTER_API_KEY=... ./build/debug/cai_example_openrouter_response
```

## Conversation Handles

Create a conversation handle transparently and run a session against it:

```sh
cmake --build --preset debug --target cai_example_conversation_handles
./build/debug/cai_example_conversation_handles
```

Reuse an existing OpenAI conversation ID without threading IDs through every
call:

```sh
./build/debug/cai_example_conversation_handles conv_abc123
```

Set `CAI_EXAMPLE_MODEL` to override the example model. The default is
`CAI_MODEL_GPT_5_NANO`.

## Streaming Text

Read response text from a pipe-backed `cai_source` while the SSE response is
still being generated:

```sh
cmake --build --preset debug --target cai_example_streaming_text
./build/debug/cai_example_streaming_text
```

## History Export

Run one non-streamed agent turn with opt-in local history capture, then stream
the exported history JSON array to stdout through `cai_source_copy_to_sink`:

```sh
cmake --build --preset debug --target cai_example_history_export
./build/debug/cai_example_history_export
```

## Session State

Run one session turn, write a versioned session state JSON file, restore a new
session from that file, and continue inference through the restored
continuation handle:

```sh
cmake --build --preset debug --target cai_example_session_state
./build/debug/cai_example_session_state /tmp/cai-session-state.json
```

## SMHI Weather Tool

Run an agent with a typed lonejson tool that receives a location name, resolves
it through the Open-Meteo Geocoding API, calls SMHI Open Data's public `snow1g`
point forecast API for the resolved coordinate, parses both JSON responses
through lonejson maps, and returns typed weather fields for the agent to
summarize.

```sh
cmake --build --preset debug --target cai_example_smhi_weather
OPENAI_API_KEY=... ./build/debug/cai_example_smhi_weather Gothenburg
```

## MCP Server

Run a small MCP-over-HTTP example server on loopback. It streams request bodies
through `cai_mcp_handler`, registers the production `reverse_geocode` and
`todo_kanban` presets, and registers `copy_to_clipboard` only when `xclip` is
available in `PATH`. Clipboard writes are a local-machine side effect and the
example rejects clipboard inputs larger than 1 MiB.

The server runs in the foreground. It logs with libpslog to `stderr` so
`--print-port` remains machine-readable on `stdout`; the MCP server logger and
the cai client logger use separate palettes.

The todo store defaults to cai's normal todo path. For isolated runs, set
`CAI_MCP_EXAMPLE_TODO_STORE` and `CAI_MCP_EXAMPLE_TODO_LOCK`.

```sh
cmake --build --preset debug --target cai_example_mcp_server
./build/debug/cai_example_mcp_server --port 18766
```

## SearXNG Search Tool

Run an agent with the built-in SearXNG search preset. Start the local SearXNG
service first with `make searxng-up`. The example leaves `config.engine` unset
by default so the SearXNG instance uses its configured search engines. Set
`CAI_SEARXNG_ENGINE` to force a specific engine.

```sh
cmake --build --preset debug --target cai_example_searxng_search
OPENAI_API_KEY=... ./build/debug/cai_example_searxng_search "OpenAI Responses API"
CAI_SEARXNG_ENGINE=wikipedia OPENAI_API_KEY=... ./build/debug/cai_example_searxng_search "OpenAI"
```

## Terminal Chat

Run a small terminal chat agent that reads prompts from stdin, keeps context
through cai's `previous_response_id` session mode with server-side
auto-compaction enabled by default, streams reasoning summaries and response
tokens to stdout, uses a stable `prompt_cache_key` for OpenAI prompt-cache
bucketing, and prints token usage plus context window percentage and estimated
cumulative USD cost to stderr after each turn. Cost is estimated locally from
model pricing metadata and response usage; it is not a billing-grade invoice
value. The chat agent registers the SearXNG search preset and can call it when
it needs current or external information; start local SearXNG with
`make searxng-up`. Tool calls are printed with `[tool]` input and output lines
so search activity is visible while a turn is running. Exit with Ctrl-D at an
empty prompt, `/quit`, or `/exit`.

```sh
cmake --build --preset debug --target cai_example_terminal_chat
OPENAI_API_KEY=... ./build/debug/cai_example_terminal_chat
```

## Mike Mind

Run a terminal chat agent seeded from an embedded first-person Mike-style
developer prompt. The example is self-contained at runtime, does not read
external files, and uses `[reasoning]` plus `[mike]` stream labels. The agent
sets a stable `prompt_cache_key` because the developer prompt is cacheable
across runs.

```sh
cmake --build --preset debug --target cai_example_mike_mind
OPENAI_API_KEY=... ./build/debug/cai_example_mike_mind
```
