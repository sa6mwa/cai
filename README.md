# cai

`cai` is a C89/POSIX SDK-style client for the OpenAI Responses API,
Conversations, Responses WebSocket mode, and Realtime WebSocket workflows.

The goal is a small, handle-oriented C API that fits systems such as Vectis:
stream-first data flow, explicit ownership, predictable error handling, and
good ergonomics for agentic workflows without forcing application code to build
raw HTTP requests or hand-roll JSON.

See [PLAN.md](PLAN.md) for the current implementation plan.

## Status

This project is under active implementation. The API surface is still expected
to change while the Responses, Conversations, streaming, tool, compaction, and
example layers settle.

## Design Bias

- C89 with POSIX features.
- CMake/Ninja/Make based build.
- OpenAI API key from explicit config, `.env`, or `OPENAI_API_KEY`.
- `.env` overrides the inherited environment when present.
- `lonejson` is the JSON layer.
- Large inputs and outputs should stream through lonejson spooling/source/sink
  paths rather than being materialized in memory.
- Production SDK calls should choose a model explicitly.
- Examples and live development tests default to `gpt-5-nano`.

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
copy; they are operational metadata. Without them, a client cannot reliably
decide when to compact, cannot report context-window usage as a percentage,
cannot validate whether a requested feature is supported, and cannot produce
good local diagnostics before an API call fails remotely.

As a result, `cai` will treat model metadata as curated SDK data:

- Model constants are strings, not a restrictive enum.
- Unknown model strings are allowed by default so new OpenAI models are usable
  before `cai` is updated.
- Known models get a compiled metadata table with capability flags and context
  limits.
- Metadata rows must distinguish verified, incomplete, inferred, deprecated,
  and unknown data.
- Runtime `/v1/models/{model}` checks may be used for availability, but not for
  context windows or feature discovery because the API does not return those
  facts.
- Auto-compaction must require known context metadata or an explicit caller
  threshold. It should not guess.

In practice this means `cai` will likely need a reviewed generated metadata file
built from official OpenAI documentation/model comparison data. That is
unfortunate, but it is the only way to make auto-compaction, context usage
reporting, and feature validation useful until OpenAI ships a proper model
registry.

Relevant OpenAI documentation:

- <https://platform.openai.com/docs/api-reference/models/list>
- <https://platform.openai.com/docs/models/compare>
- <https://platform.openai.com/docs/api-reference/responses>

