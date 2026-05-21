#ifndef CAI_MODELS_H
#define CAI_MODELS_H

#ifdef __cplusplus
extern "C" {
#endif

/** OpenAI Responses model id constants. */
#define CAI_MODEL_GPT_5_5 "gpt-5.5"
#define CAI_MODEL_GPT_5_5_2026_04_23 "gpt-5.5-2026-04-23"
#define CAI_MODEL_GPT_5_5_PRO "gpt-5.5-pro"
#define CAI_MODEL_GPT_5_5_PRO_2026_04_23 "gpt-5.5-pro-2026-04-23"
#define CAI_MODEL_GPT_5_4_NANO "gpt-5.4-nano"
#define CAI_MODEL_GPT_5_4_MINI "gpt-5.4-mini"
#define CAI_MODEL_GPT_5_4 "gpt-5.4"
#define CAI_MODEL_GPT_5_4_MINI_2026_03_17 "gpt-5.4-mini-2026-03-17"
#define CAI_MODEL_GPT_5_4_NANO_2026_03_17 "gpt-5.4-nano-2026-03-17"
#define CAI_MODEL_GPT_5_3_CHAT_LATEST "gpt-5.3-chat-latest"
#define CAI_MODEL_GPT_5_2 "gpt-5.2"
#define CAI_MODEL_GPT_5_2_2025_12_11 "gpt-5.2-2025-12-11"
#define CAI_MODEL_GPT_5_2_CHAT_LATEST "gpt-5.2-chat-latest"
#define CAI_MODEL_GPT_5_2_PRO "gpt-5.2-pro"
#define CAI_MODEL_GPT_5_2_PRO_2025_12_11 "gpt-5.2-pro-2025-12-11"
#define CAI_MODEL_GPT_5_1 "gpt-5.1"
#define CAI_MODEL_GPT_5_1_2025_11_13 "gpt-5.1-2025-11-13"
#define CAI_MODEL_GPT_5_1_CODEX "gpt-5.1-codex"
#define CAI_MODEL_GPT_5_1_CODEX_MAX "gpt-5.1-codex-max"
#define CAI_MODEL_GPT_5_1_MINI "gpt-5.1-mini"
#define CAI_MODEL_GPT_5_1_CHAT_LATEST "gpt-5.1-chat-latest"
#define CAI_MODEL_GPT_5 "gpt-5"
#define CAI_MODEL_GPT_5_MINI "gpt-5-mini"
#define CAI_MODEL_GPT_5_NANO "gpt-5-nano"
#define CAI_MODEL_GPT_5_2025_08_07 "gpt-5-2025-08-07"
#define CAI_MODEL_GPT_5_MINI_2025_08_07 "gpt-5-mini-2025-08-07"
#define CAI_MODEL_GPT_5_NANO_2025_08_07 "gpt-5-nano-2025-08-07"
#define CAI_MODEL_GPT_5_CHAT_LATEST "gpt-5-chat-latest"
#define CAI_MODEL_GPT_5_CODEX "gpt-5-codex"
#define CAI_MODEL_GPT_5_PRO "gpt-5-pro"
#define CAI_MODEL_GPT_5_PRO_2025_10_06 "gpt-5-pro-2025-10-06"
#define CAI_MODEL_GPT_4_1 "gpt-4.1"
#define CAI_MODEL_GPT_4_1_MINI "gpt-4.1-mini"
#define CAI_MODEL_GPT_4_1_NANO "gpt-4.1-nano"
#define CAI_MODEL_GPT_4_1_2025_04_14 "gpt-4.1-2025-04-14"
#define CAI_MODEL_GPT_4_1_MINI_2025_04_14 "gpt-4.1-mini-2025-04-14"
#define CAI_MODEL_GPT_4_1_NANO_2025_04_14 "gpt-4.1-nano-2025-04-14"
#define CAI_MODEL_O4_MINI "o4-mini"
#define CAI_MODEL_O4_MINI_2025_04_16 "o4-mini-2025-04-16"
#define CAI_MODEL_O4_MINI_DEEP_RESEARCH "o4-mini-deep-research"
#define CAI_MODEL_O4_MINI_DEEP_RESEARCH_2025_06_26                        \
  "o4-mini-deep-research-2025-06-26"
#define CAI_MODEL_O3 "o3"
#define CAI_MODEL_O3_2025_04_16 "o3-2025-04-16"
#define CAI_MODEL_O3_MINI "o3-mini"
#define CAI_MODEL_O3_MINI_2025_01_31 "o3-mini-2025-01-31"
#define CAI_MODEL_O3_PRO "o3-pro"
#define CAI_MODEL_O3_PRO_2025_06_10 "o3-pro-2025-06-10"
#define CAI_MODEL_O3_DEEP_RESEARCH "o3-deep-research"
#define CAI_MODEL_O3_DEEP_RESEARCH_2025_06_26                              \
  "o3-deep-research-2025-06-26"
#define CAI_MODEL_O1 "o1"
#define CAI_MODEL_O1_2024_12_17 "o1-2024-12-17"
#define CAI_MODEL_O1_PRO "o1-pro"
#define CAI_MODEL_O1_PRO_2025_03_19 "o1-pro-2025-03-19"
#define CAI_MODEL_O1_PREVIEW "o1-preview"
#define CAI_MODEL_O1_PREVIEW_2024_09_12 "o1-preview-2024-09-12"
#define CAI_MODEL_O1_MINI "o1-mini"
#define CAI_MODEL_O1_MINI_2024_09_12 "o1-mini-2024-09-12"
#define CAI_MODEL_GPT_4O "gpt-4o"
#define CAI_MODEL_GPT_4O_2024_11_20 "gpt-4o-2024-11-20"
#define CAI_MODEL_GPT_4O_2024_08_06 "gpt-4o-2024-08-06"
#define CAI_MODEL_GPT_4O_2024_05_13 "gpt-4o-2024-05-13"
#define CAI_MODEL_GPT_4O_AUDIO_PREVIEW "gpt-4o-audio-preview"
#define CAI_MODEL_GPT_4O_AUDIO_PREVIEW_2024_10_01                          \
  "gpt-4o-audio-preview-2024-10-01"
#define CAI_MODEL_GPT_4O_AUDIO_PREVIEW_2024_12_17                          \
  "gpt-4o-audio-preview-2024-12-17"
#define CAI_MODEL_GPT_4O_AUDIO_PREVIEW_2025_06_03                          \
  "gpt-4o-audio-preview-2025-06-03"
#define CAI_MODEL_GPT_4O_MINI_AUDIO_PREVIEW "gpt-4o-mini-audio-preview"
#define CAI_MODEL_GPT_4O_MINI_AUDIO_PREVIEW_2024_12_17                     \
  "gpt-4o-mini-audio-preview-2024-12-17"
#define CAI_MODEL_GPT_4O_SEARCH_PREVIEW "gpt-4o-search-preview"
#define CAI_MODEL_GPT_4O_MINI_SEARCH_PREVIEW "gpt-4o-mini-search-preview"
#define CAI_MODEL_GPT_4O_SEARCH_PREVIEW_2025_03_11                         \
  "gpt-4o-search-preview-2025-03-11"
#define CAI_MODEL_GPT_4O_MINI_SEARCH_PREVIEW_2025_03_11                    \
  "gpt-4o-mini-search-preview-2025-03-11"
#define CAI_MODEL_CHATGPT_4O_LATEST "chatgpt-4o-latest"
#define CAI_MODEL_CODEX_MINI_LATEST "codex-mini-latest"
#define CAI_MODEL_GPT_4O_MINI "gpt-4o-mini"
#define CAI_MODEL_GPT_4O_MINI_2024_07_18 "gpt-4o-mini-2024-07-18"
#define CAI_MODEL_GPT_4_TURBO "gpt-4-turbo"
#define CAI_MODEL_GPT_4_TURBO_2024_04_09 "gpt-4-turbo-2024-04-09"
#define CAI_MODEL_GPT_4_0125_PREVIEW "gpt-4-0125-preview"
#define CAI_MODEL_GPT_4_TURBO_PREVIEW "gpt-4-turbo-preview"
#define CAI_MODEL_GPT_4_1106_PREVIEW "gpt-4-1106-preview"
#define CAI_MODEL_GPT_4_VISION_PREVIEW "gpt-4-vision-preview"
#define CAI_MODEL_GPT_4 "gpt-4"
#define CAI_MODEL_GPT_4_0314 "gpt-4-0314"
#define CAI_MODEL_GPT_4_0613 "gpt-4-0613"
#define CAI_MODEL_GPT_4_32K "gpt-4-32k"
#define CAI_MODEL_GPT_4_32K_0314 "gpt-4-32k-0314"
#define CAI_MODEL_GPT_4_32K_0613 "gpt-4-32k-0613"
#define CAI_MODEL_GPT_3_5_TURBO "gpt-3.5-turbo"
#define CAI_MODEL_GPT_3_5_TURBO_16K "gpt-3.5-turbo-16k"
#define CAI_MODEL_GPT_3_5_TURBO_0301 "gpt-3.5-turbo-0301"
#define CAI_MODEL_GPT_3_5_TURBO_0613 "gpt-3.5-turbo-0613"
#define CAI_MODEL_GPT_3_5_TURBO_1106 "gpt-3.5-turbo-1106"
#define CAI_MODEL_GPT_3_5_TURBO_0125 "gpt-3.5-turbo-0125"
#define CAI_MODEL_GPT_3_5_TURBO_16K_0613 "gpt-3.5-turbo-16k-0613"
#define CAI_MODEL_COMPUTER_USE_PREVIEW "computer-use-preview"
#define CAI_MODEL_COMPUTER_USE_PREVIEW_2025_03_11                          \
  "computer-use-preview-2025-03-11"
/** OpenRouter model id constants used by examples and integration tests. */
#define CAI_OPENROUTER_MODEL_NVIDIA_NEMOTRON_3_NANO_OMNI_30B_A3B_REASONING_FREE \
  "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free"
#define CAI_OPENROUTER_MODEL_FREE_ROUTER "openrouter/free"
#define CAI_OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE                       \
  "poolside/laguna-xs.2:free"
/** Default OpenAI Responses model used by examples and tests. */
#define CAI_MODEL_DEFAULT_RESPONSES CAI_MODEL_GPT_5_NANO
/** Default OpenRouter Responses-compatible model used by examples/tests. */
#define CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES                              \
  CAI_OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE

/** Model capability flags returned by cai_model_info and cai_model_supports. */
#define CAI_MODEL_CAP_RESPONSES 0x0001u
#define CAI_MODEL_CAP_REALTIME 0x0002u
#define CAI_MODEL_CAP_STREAMING 0x0004u
#define CAI_MODEL_CAP_FUNCTION_CALLING 0x0008u
#define CAI_MODEL_CAP_STRUCTURED_OUTPUTS 0x0010u
#define CAI_MODEL_CAP_IMAGE_INPUT 0x0020u
#define CAI_MODEL_CAP_AUDIO_INPUT 0x0040u
#define CAI_MODEL_CAP_AUDIO_OUTPUT 0x0080u
/** Model metadata flags describing bundled model metadata confidence. */
#define CAI_MODEL_META_VERIFIED 0x0001u
#define CAI_MODEL_META_INCOMPLETE 0x0002u
#define CAI_MODEL_META_INFERRED 0x0004u
#define CAI_MODEL_META_DEPRECATED 0x0008u
#define CAI_MODEL_META_PROVIDER_OPENROUTER 0x0010u
/** Static metadata for a known model id. */
typedef struct cai_model_info {
  /** Model id string. */
  const char *id;
  /** ORed CAI_MODEL_CAP_* capability flags. */
  unsigned int capabilities;
  /** ORed CAI_MODEL_META_* metadata flags. */
  unsigned int metadata_flags;
  /** Known or inferred context window in tokens, or zero if unknown. */
  long long context_window_tokens;
  /** Default server-side compaction threshold in tokens, or zero. */
  long long auto_compact_token_limit;
  /** Estimated uncached input price in USD per million tokens. */
  double input_usd_per_million;
  /** Estimated cached input price in USD per million tokens. */
  double cached_input_usd_per_million;
  /** Estimated output price in USD per million tokens. */
  double output_usd_per_million;
} cai_model_info;

/** Look up bundled metadata for a model id. */
const cai_model_info *cai_model_info_by_id(const char *model_id);
/** Return non-zero if a model has the requested CAI_MODEL_CAP_* flag. */
int cai_model_supports(const char *model_id, unsigned int capability);
/** Return CAI_MODEL_META_* flags for a model id, or zero if unknown. */
unsigned int cai_model_metadata_flags(const char *model_id);
/** Return the known context window for a model id, or zero if unknown. */
long long cai_model_context_window_tokens(const char *model_id);
/** Return the default compaction threshold for a model id, or zero. */
long long cai_model_auto_compact_token_limit(const char *model_id);
/** Estimate USD cost from token usage using bundled model pricing. */
double cai_model_estimate_usage_usd(const char *model_id,
                                    long long input_tokens,
                                    long long input_cached_tokens,
                                    long long output_tokens);

#ifdef __cplusplus
}
#endif

#endif
