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

Remaining follow-up:

- Run explicit OpenRouter integration tests before claiming parity for
  Conversations or server-side compaction semantics.
