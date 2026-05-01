# TODO

Additions to [PLAN.md](PLAN.md)

- [x] Support openrouter.ai, we have OPENROUTER_API_KEY for https://openrouter.ai/api/v1
- [x] Find the cheapest model supporting the responses API via openrouter.ai
- [x] Investigate if this model via openrouter supports the responses API: https://openrouter.ai/nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free

Implemented first slice:

- `cai_client_config_use_openrouter()` selects `OPENROUTER_API_KEY` and
  `https://openrouter.ai/api/v1`.
- `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` is
  `nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free`.
- OpenRouter registry data currently reports that model as zero-price for
  prompt/completion tokens, 256k context, and supporting Responses, reasoning,
  and tool calling.
- `CAI_INTEGRATION_OPENROUTER_DOTENV=1` verifies the OpenRouter `.env` key
  loading path by clearing the inherited `OPENROUTER_API_KEY` process envvar
  before opening the client.

Current OpenRouter boundary:

- Do not claim OpenRouter parity for Conversations or server-side compaction
  semantics until those specific behaviors are documented or separately proven.
- OpenRouter Responses beta is stateless, so cai uses client-side history
  replay for multi-turn OpenRouter sessions. Server-side continuation remains
  the cai default for OpenAI.
