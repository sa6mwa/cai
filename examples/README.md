# CAI Examples

These examples are built by default with `CAI_BUILD_EXAMPLES=ON`. Most call the
real OpenAI API when run, so they require either `OPENAI_API_KEY` in the
environment, a repo-local `.env` file containing `OPENAI_API_KEY=...`, or an
explicit ChatGPT subscription auth file for examples that support
`CAI_CHATGPT_AUTH_JSON`. The examples load dotenv files explicitly and pass the
parsed key as `cai_client_config.api_key` or `cai.open({ api_key = ... })`.

`cai_client_open` itself does not implicitly load dotenv files. Applications
that want dotenv support should call `cai_load_dotenv_api_key` explicitly, pass
the returned key to `cai_client_config.api_key`, then release it with
`cai_string_destroy` after opening the client. Lua callers can use
`cai.load_dotenv_api_key(path, env_name)` and pass the returned string as
`api_key`.

ChatGPT subscription login is also explicit. Run `run-chatgpt-login` to create
a Codex-style auth file through the browser OAuth flow, then pass that same
path to examples that support `CAI_CHATGPT_AUTH_JSON`.

The agent-oriented examples use the method-style handle facade (`client->...`,
`agent->...`, `session->...`). Raw Responses examples still use free functions
because they demonstrate request construction close to the wire API.

Most examples can be built and run through the examples Makefile:

```sh
make -C examples help
```

For automated verification, the repo exposes two narrower smoke paths:

- `make example-smoke-local` covers deterministic local example checks.
- `CAI_ENABLE_INTEGRATION_TESTS=1 make example-smoke-live` runs a curated
  non-interactive live subset against the real API.

## Basic Response

```sh
make -C examples run-basic-response
```

## ChatGPT Login

Start a local callback listener, open the ChatGPT OAuth URL, and write a
Codex-style auth file:

```sh
make -C examples run-chatgpt-login CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json
```

Use `CAI_CHATGPT_LOGIN_PORT=1457` or another free local port if the default
callback port is unavailable. The example is intentionally interactive; local
unit tests mock the callback and token exchange flow.

## OpenRouter Response

Run the same Responses-style request through OpenRouter. This example uses
`OPENROUTER_API_KEY`, `https://openrouter.ai/api/v1`, and
`CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES`. Set `CAI_OPENROUTER_EXAMPLE_MODEL` to
override the model.

```sh
OPENROUTER_API_KEY=... make -C examples run-openrouter-response
```

## Conversation Handles

Create a conversation handle transparently and run a session against it:

```sh
make -C examples run-conversation-handles
```

Reuse an existing OpenAI conversation ID without threading IDs through every
call:

```sh
cmake --build --preset debug --target cai_example_conversation_handles
./build/debug/cai_example_conversation_handles conv_abc123
```

Set `CAI_EXAMPLE_MODEL` to override the example model. The default is
`CAI_MODEL_GPT_5_NANO`.

## Streaming Text

Read response text from a pipe-backed `cai_source` while the SSE response is
still being generated:

```sh
make -C examples run-streaming-text
```

## History Export

Run one non-streamed agent turn with opt-in local history capture, then stream
the exported history JSON array to stdout through `cai_source_copy_to_sink`:

```sh
make -C examples run-history-export
```

## Session State

Run one session turn, write a versioned session state JSON file, restore a new
session from that file, and continue inference through the restored
continuation handle:

```sh
make -C examples run-session-state CAI_SESSION_STATE_PATH=/tmp/cai-session-state.json
```

## SMHI Weather Tool

Run an agent with a typed lonejson tool that receives a location name, resolves
it through the Open-Meteo Geocoding API, calls SMHI Open Data's public `snow1g`
point forecast API for the resolved coordinate, parses both JSON responses
through lonejson maps, streams Open-Meteo `results` and SMHI `timeSeries`
arrays item by item, and returns typed weather fields for the agent to
summarize. This is an example-local tool, not a public cai tool preset.

```sh
OPENAI_API_KEY=... make -C examples run-smhi-weather CAI_SMHI_LOCATION=Gothenburg
```

## MCP Server

Run a small MCP-over-HTTP example server on loopback. It streams request bodies
through `cai_mcp_handler`, registers the production `reverse_geocode` and
`todo_kanban` presets, and registers `copy_to_clipboard` only when `xclip` is
available in `PATH`. Clipboard writes are a local-machine side effect and the
example rejects clipboard inputs larger than 1 MiB. The same `/mcp` endpoint
supports JSON replies, SSE-only POST replies, and a lightweight GET SSE stream
heartbeat.

The server runs in the foreground. It logs with libpslog to `stderr` so
`--print-port` remains machine-readable on `stdout`; the MCP server logger and
the cai client logger use separate palettes.

The todo store defaults to cai's normal todo path. For isolated runs, set
`CAI_MCP_EXAMPLE_TODO_STORE` and `CAI_MCP_EXAMPLE_TODO_LOCK`.

```sh
make -C examples run-mcp-server CAI_MCP_EXAMPLE_PORT=18766
```

See `examples/mcp-server/README.md` for curl probes, diagnostic endpoints, and
MCP Inspector notes.

## SearXNG Search Tool

Run an agent with the built-in SearXNG search preset. Start the local SearXNG
service first with `make searxng-up`. The example leaves `config.engine` unset
by default so the SearXNG instance uses its configured search engines. Set
`CAI_SEARXNG_ENGINE` to force a specific engine.

```sh
OPENAI_API_KEY=... make -C examples run-searxng-search CAI_SEARXNG_QUERY="OpenAI Responses API"
CAI_SEARXNG_ENGINE=wikipedia OPENAI_API_KEY=... make -C examples run-searxng-search CAI_SEARXNG_QUERY="OpenAI"
```

## Lua Basic

Build the local LuaRock into `build/luarocks`, then run a Lua 5.5 example
against the same C SDK facade:

```sh
OPENAI_API_KEY=... make -C examples run-lua-basic
```

Lua examples use `require("cai")` and the LuaRock module built by
`make lua-rock`. The rock depends on the `lonejson` Lua rock; `make lua-rock`
installs the required lonejson source rock from the official GitHub release
into `build/luarocks` when needed.

## Lua Terminal Chat

Lua port of the terminal chat agent. It streams reasoning and response text,
prints streamed tool input/output, prints usage, registers SearXNG search, and
registers the persisted `todo_kanban` tool preset. The Lua binding also exposes
spooled large-value methods such as `add_user_text_spooled()` and
`register_raw_spooled_tool()` for lonejson-backed payloads. Command execution is
not enabled by default. Pass `--exec-tool-dir <path>` to register
`exec_command` rooted to that path. File inspection is also opt-in; pass
`--read-tool-dir <path>` to register `list_files` and `read_file` rooted to
that path. `list_files` reports `text_candidate`/`binary_candidate` hints for
regular files, and `read_file` only returns UTF-8 text without unsafe control
characters.

```sh
OPENAI_API_KEY=... make -C examples run-lua-terminal-chat
make -C examples run-lua-terminal-chat CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json
OPENAI_API_KEY=... make -C examples run-lua-terminal-chat CAI_EXEC_TOOL_DIR=/tmp/cai-exec-root
OPENAI_API_KEY=... make -C examples run-lua-terminal-chat CAI_READ_TOOL_DIR="$PWD"
```

Optional local todo isolation:

```sh
CAI_LUA_TODO_STORE=/tmp/cai-lua-todo.json \
CAI_LUA_TODO_LOCK=/tmp/cai-lua-todo.lock \
OPENAI_API_KEY=... make -C examples run-lua-terminal-chat
```

## Lua Conversation

Create a conversation handle and add items through the lower-level
Conversations facade:

```sh
OPENAI_API_KEY=... make -C examples run-lua-conversation
```

## Lua Session State

Run a turn, save session state to disk, restore it into a new session, and
continue:

```sh
OPENAI_API_KEY=... make -C examples run-lua-session-state CAI_LUA_SESSION_STATE_PATH=/tmp/cai-lua-state.json
```

## Terminal Chat

Run a small terminal chat agent that reads prompts from stdin, keeps context
through cai's `previous_response_id` session mode with server-side
auto-compaction enabled by default, streams reasoning summaries and response
tokens to stdout, uses a stable `prompt_cache_key` for OpenAI prompt-cache
bucketing, and prints token usage plus context window percentage and estimated
cumulative USD cost to stderr after each turn. Cost is estimated locally from
model pricing metadata and response usage; it is not a billing-grade invoice
value. The chat agent registers the SearXNG search preset and the persisted
`todo_kanban` preset. It can search when it needs current or external
information; start local SearXNG with `make searxng-up`. It can also manage
local kanban boards when asked to remember, plan, list, move, limit, or archive
work. Tool calls are printed with `[tool]` input and output lines so activity
is visible while a turn is running. Exit with Ctrl-D at an empty prompt,
`/quit`, or `/exit`. Command execution is not enabled by default. Pass
`--exec-tool-dir <path>` to register `exec_command` rooted to that path. File
inspection is also opt-in; pass `--read-tool-dir <path>` to register
`list_files` and `read_file` rooted to that path. `list_files` reports
`text_candidate`/`binary_candidate` hints for regular files, and `read_file`
only returns UTF-8 text without unsafe control characters.

```sh
OPENAI_API_KEY=... make -C examples run-terminal-chat
OPENAI_API_KEY=... make -C examples run-terminal-chat CAI_EXEC_TOOL_DIR=/tmp/cai-exec-root
OPENAI_API_KEY=... make -C examples run-terminal-chat CAI_READ_TOOL_DIR="$PWD"
make -C examples run-terminal-chat CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json
```

Optional local todo isolation:

```sh
CAI_TODO_STORE=/tmp/cai-todo.json \
CAI_TODO_LOCK=/tmp/cai-todo.lock \
OPENAI_API_KEY=... make -C examples run-terminal-chat
```

## Mike Mind

Run a terminal chat agent seeded from an embedded first-person Mike-style
developer prompt. The example is self-contained at runtime, does not read
external files, and uses `[reasoning]` plus `[mike]` stream labels. The agent
sets a stable `prompt_cache_key` because the developer prompt is cacheable
across runs.

```sh
OPENAI_API_KEY=... make -C examples run-mike-mind
```
