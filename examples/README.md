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

## Terminal Chat

Run a small terminal chat agent that reads prompts from stdin, streams response
tokens to stdout, keeps context through cai's `previous_response_id` session
mode with server-side auto-compaction enabled by default, renders reasoning
summaries and responses in stream order on stdout when the API provides them,
uses a stable `prompt_cache_key` for OpenAI prompt-cache bucketing, and prints
token usage plus context window percentage and estimated cumulative USD cost to
stderr after each turn. Cost is estimated locally from model pricing metadata
and response usage;
it is not a billing-grade invoice value. Exit with Ctrl-D at an empty prompt,
`/quit`, or `/exit`.

```sh
cmake --build --preset debug --target cai_example_terminal_chat
OPENAI_API_KEY=... ./build/debug/cai_example_terminal_chat
```

## Mike Mind

Run a terminal chat agent seeded from the Mike Mind skill references. By default
the example reads `../parallax/skills/mike-mind`; override that with
`CAI_MIKE_MIND_SKILL_DIR` when running from another checkout layout. The agent
sets a stable `prompt_cache_key` because the skill prompt is intentionally large
and cacheable across runs.

```sh
cmake --build --preset debug --target cai_example_mike_mind
OPENAI_API_KEY=... ./build/debug/cai_example_mike_mind
```
