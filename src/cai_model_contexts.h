#ifndef CAI_MODEL_CONTEXTS_H
#define CAI_MODEL_CONTEXTS_H

/* Model-card token limits used only when an endpoint catalog omits context.
 * Keep this table limited to published model limits; the live provider API
 * owns model availability, capabilities, lifecycle, and prices. Serving limits
 * may be lower, so provider-supplied values always take precedence.
 * Update a row when the model owner publishes a new limit.
 */
typedef struct cai_model_context_seed {
  const char *id;
  long long tokens;
  const char *source;
} cai_model_context_seed;

static const cai_model_context_seed cai_berget_contexts[] = {
    {"Qwen/Qwen3.8-27B-FP8", 262144LL,
     "https://huggingface.co/Qwen/Qwen3.8-27B-FP8"},
    {"mistralai/Mistral-Small-3.2-24B-Instruct-2506", 131072LL,
     "https://huggingface.co/mistralai/Mistral-Small-3.2-24B-Instruct-2506/"
     "blob/main/config.json"},
    {"zai-org/GLM-5.3-Flash", 1048576LL,
     "https://huggingface.co/zai-org/GLM-5.3-Flash/blob/main/config.json"},
    {"moonshotai/Kimi-K3", 1048576LL,
     "https://huggingface.co/moonshotai/Kimi-K3/blob/main/config.json"},
    {"google/gemma-4-31B-it", 262144LL,
     "https://huggingface.co/google/gemma-4-31B-it/blob/main/config.json"},
    {"BAAI/bge-reranker-v2-m3", 8192LL,
     "https://huggingface.co/BAAI/bge-reranker-v2-m3"},
    {"Qwen/Qwen3.5-2B", 262144LL, "https://huggingface.co/Qwen/Qwen3.5-2B"},
    {"convaiinnovations/laya", 512LL,
     "https://huggingface.co/convaiinnovations/laya"},
    {"intfloat/multilingual-e5-large-instruct", 512LL,
     "https://huggingface.co/intfloat/multilingual-e5-large-instruct"},
    {"intfloat/multilingual-e5-large", 512LL,
     "https://huggingface.co/intfloat/multilingual-e5-large"}};

#endif
