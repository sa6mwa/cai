# Model Metadata Maintenance

## Provider catalog fallback

`cai_client_list_models` fetches the live ChatGPT or Berget catalog when
available. Berget's `GET /v1/models` currently includes text, rerank,
embedding, speech-to-text, and system-one models. The adapter includes every
returned model; `model_type` identifies which ones are usable for chat. The
catalog is cached for five minutes per client. Other providers can use the
JSON fallback catalog directly.

The CLI reads `$XDG_CONFIG_HOME/cai/models.json`, falling back to
`$HOME/.config/cai/models.json`. `--config-dir` changes the base directory;
`--model-metadata FILE` selects a specific file. A missing default file is
allowed. libcai callers can supply the same JSON through
`cai_client_config.model_metadata_json`; libcai copies and validates it during
client creation. Lua callers can pass `model_metadata_json` to `cai.open`.
The schema is:

```json
{
  "models": [
    {
      "provider": "berget",
      "id": "zai-org/GLM-5.3-Flash",
      "context_window_tokens": 131072,
      "auto_compact_token_limit": 104857,
      "capabilities": { "streaming": true, "vision": true, "function_calling": true },
      "pricing": { "currency": "EUR", "input": 0.2, "output": 0.8 }
    }
  ]
}
```

Prices default to per million tokens. `pricing.unit` can specify another
unit, such as `second`; these prices remain visible in the catalog but do not
drive token spend estimates. These example values illustrate the file
format; verify actual limits and prices with the provider before using them.
Each entry needs `provider` and `id`; other fields are optional. Optional
string fields are `name`, `description`, `model_type`, `lifecycle_status`, and
`comp_hash`. Optional capability booleans are `streaming`,
`function_calling`, `vision`, `formatted_output`, and `json_mode`. Only `USD`
and `EUR` token prices are used for estimates. Live provider fields take
precedence; the file fills fields omitted by the provider, including a missing
context window. An explicit live `false` capability remains false. When a
live catalog is available, the fallback does not add models that the provider
does not advertise. A fallback-only catalog is returned when live discovery
fails or the provider has no catalog endpoint.
Verified bundled OpenAI/OpenRouter USD prices remain authoritative for spend
accounting even when a fallback file names the same model.

Berget token estimates remain in EUR. CAI does not convert them to USD.
Positive USD spend limits fail closed for models with only EUR pricing.
Provider prices are estimates for text tokens and are not a billing record.

CAI keeps model metadata as curated SDK data because the OpenAI Models API does
not expose enough operational information for a C SDK. Model constants stay as
plain strings so callers can use new remote models before CAI knows about them,
but known models should carry verified metadata when the data is available from
official documentation.

## Sources

Use official OpenAI documentation only:

- <https://developers.openai.com/api/docs/models>
- <https://developers.openai.com/api/docs/pricing>
- <https://platform.openai.com/docs/api-reference/models/list>
- <https://platform.openai.com/docs/api-reference/responses>

Do not copy pricing or capability data from blog posts, third-party tables,
forum answers, generated summaries, or provider dashboards unless the provider
is not OpenAI and that provider's own official documentation is the source.

When official docs disagree, prefer the per-model page for capabilities and
context windows, and the pricing page for price tables. Leave a model incomplete
when the conflict cannot be resolved from official sources.

## What To Update

For every newly added or changed OpenAI model:

1. Add or update string constants in `include/cai/models.h`.
2. Add or update the row in `src/cai_models.c`.
3. Set capability flags from the per-model page:
   - Responses support.
   - Streaming support.
   - Function calling support.
   - Structured output support.
   - Image/audio input and output support.
   - Realtime support, when CAI supports that surface.
   - Reasoning support and any model-specific reasoning modes.
4. Set context-window metadata from the per-model page.
5. Set pricing metadata from the Standard API pricing table.
6. Add long-context pricing fields when OpenAI documents a specific threshold.
7. Expose new public constants and metadata fields in `lua/cai_lua.c`.
8. Extend C and Lua tests to cover the new constants, context windows,
   capabilities, pricing, and spend-estimation behavior.
9. Update `README.md` if the public accounting behavior changes.

## Pricing Policy

CAI's bundled USD estimator is for local spend-limit enforcement, not billing
reconciliation. It intentionally uses the direct Standard API text-token prices
per 1M tokens.

Rules:

- Do not encode Batch, Flex, Priority, or data-residency uplift prices in the
  default estimator.
- Do not estimate hosted-tool per-call fees, media generation, audio minutes,
  embeddings, moderation, or other non-text-token surfaces in
  `cai_model_estimate_usage_usd` until those accounting surfaces are explicitly
  modeled.
- If `max_spend_usd` is positive, CAI must fail closed when pricing metadata is
  missing or incomplete.
- Verified free OpenRouter rows may estimate `$0.00` only when the provider row
  is explicitly marked verified and provider-specific.
- If a model's official pricing table has no cached-input discount and the
  token-billing semantics are otherwise clear, store cached input at the same
  price as uncached input. If that is ambiguous, leave the model unpriced.
- For long-context pricing, store the documented absolute token threshold and
  the long-context prices. Do not infer a threshold from a context-window
  percentage.

For GPT-5.4/GPT-5.5 style long-context pricing, OpenAI documents that prompts
above the threshold use the higher input and output prices for the full
session. CAI therefore switches all input, cached input, and output token
pricing to the long-context prices when `input_tokens` is above the documented
threshold.

## Metadata Flags

Use `CAI_MODEL_META_VERIFIED` only when the row has been checked against
official docs during the update. Use `CAI_MODEL_META_INCOMPLETE` when a known
model id is useful to expose but key facts are missing or ambiguous.

Unknown model strings remain accepted by request-building APIs. They simply do
not get local capability validation, auto-compaction defaults, or positive USD
spend-limit enforcement.

## Verification

Run the focused checks while editing:

```sh
make format
make build-debug
build/debug/cai_tests --filter model_capabilities
build/debug/cai_tests --filter usage_
make lua-rock
eval "$(make -s lua-env)" && lua tests/lua/test_lua.lua
```

Before committing a model/pricing update, run formatting and the local suite:

```sh
make format
make test-debug CTEST_FLAGS='--output-on-failure'
```

For ordinary implementation slices outside model metadata, `make
finalize-slice` is the default local pre-commit shortcut. It runs `make format`
and the debug CTest suite. Add Lua, sanitizer, fuzz, integration, example, or
release gates when the slice touches those surfaces.

For a release candidate, use the normal release gates instead of only the
focused checks:

```sh
make prerelease
make prerelease-live
make prerelease-hardening
```

Live gates require valid provider credentials and intentionally spend tokens.

## Commit Shape

Model/pricing updates should be committed as their own slice with a
Conventional Commit message, for example:

```text
feat(models): update OpenAI pricing metadata
```

Keep unrelated refactors out of the same commit. If an update requires changing
the public metadata struct or Lua facade, include the matching tests in the
same commit.
