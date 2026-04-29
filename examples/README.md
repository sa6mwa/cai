# CAI Examples

These examples are built by default with `CAI_BUILD_EXAMPLES=ON`. They are live
programs: running them requires either `OPENAI_API_KEY` in the environment or a
repo-local `.env` file containing `OPENAI_API_KEY=...`.

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
`CAI_MODEL_GPT_5_4_NANO`.

## Streaming Text

Read response text from a pipe-backed `cai_source` while the SSE response is
still being generated:

```sh
cmake --build --preset debug --target cai_example_streaming_text
./build/debug/cai_example_streaming_text
```

## Terminal Chat

Run a small terminal chat agent that reads prompts from stdin, streams response
tokens to stdout, keeps context through cai's history-backed session mode, and
prints token usage plus context window percentage to stderr after each turn.
Exit with Ctrl-D at an empty prompt, `/quit`, or `/exit`.

```sh
cmake --build --preset debug --target cai_example_terminal_chat
OPENAI_API_KEY=... ./build/debug/cai_example_terminal_chat
```
