# TODO

Additions to [PLAN.md](PLAN.md)

- [x] Support openrouter.ai, we have OPENROUTER_API_KEY for https://openrouter.ai/api/v1
- [x] Find the cheapest model supporting the responses API via openrouter.ai
- [x] Investigate if this model via openrouter supports the responses API: https://openrouter.ai/nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free
- [x] Switch the OpenRouter go-to e2e model to the free model that actually
  passes cai continuity and tool-calling regressions.

Implemented first slice:

- `cai_client_config_use_openrouter()` selects `OPENROUTER_API_KEY` and
  `https://openrouter.ai/api/v1`.
- `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` is
  `poolside/laguna-xs.2:free`.
- OpenRouter registry data currently reports that model as zero-price for
  prompt/completion tokens, 131k context, and supporting Responses, reasoning,
  and tool calling. It is the default because it passes cai's Responses,
  tool-calling, and 20-turn client-history e2e regressions.
- `CAI_INTEGRATION_OPENROUTER_DOTENV=1` verifies the OpenRouter `.env` key
  loading path by clearing the inherited `OPENROUTER_API_KEY` process envvar
  before opening the client.
- `CAI_INTEGRATION_OPENROUTER_E2E=1` runs the same 20-turn continuity eval as
  the OpenAI e2e path, using OpenRouter's default free Responses model and
  cai's client-side history replay mode.
- `nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free` remains enumerated, but
  it is not the default because it failed the 20-turn continuity e2e by
  repeating the first response on the second turn.
- `poolside/laguna-xs.2:free` is the go-to OpenRouter model for e2e testing.

Current OpenRouter boundary:

- Do not claim OpenRouter parity for Conversations or server-side compaction
  semantics until those specific behaviors are documented or separately proven.
- OpenRouter Responses beta is stateless, so cai uses client-side history
  replay for multi-turn OpenRouter sessions. Server-side continuation remains
  the cai default for OpenAI.
