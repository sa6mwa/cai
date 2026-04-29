#include <cai/models.h>

#include <string.h>

static const cai_model_info cai_models[] = {
    {CAI_MODEL_GPT_5_4_NANO,
     CAI_MODEL_CAP_RESPONSES | CAI_MODEL_CAP_REALTIME |
         CAI_MODEL_CAP_STREAMING | CAI_MODEL_CAP_FUNCTION_CALLING |
         CAI_MODEL_CAP_STRUCTURED_OUTPUTS | CAI_MODEL_CAP_IMAGE_INPUT,
     400000LL, 320000LL},
    {CAI_MODEL_GPT_5_4_MINI,
     CAI_MODEL_CAP_RESPONSES | CAI_MODEL_CAP_REALTIME |
         CAI_MODEL_CAP_STREAMING | CAI_MODEL_CAP_FUNCTION_CALLING |
         CAI_MODEL_CAP_STRUCTURED_OUTPUTS | CAI_MODEL_CAP_IMAGE_INPUT,
     400000LL, 320000LL},
    {CAI_MODEL_GPT_5_4,
     CAI_MODEL_CAP_RESPONSES | CAI_MODEL_CAP_REALTIME |
         CAI_MODEL_CAP_STREAMING | CAI_MODEL_CAP_FUNCTION_CALLING |
         CAI_MODEL_CAP_STRUCTURED_OUTPUTS | CAI_MODEL_CAP_IMAGE_INPUT,
     1050000LL, 840000LL},
    {NULL, 0U, 0LL, 0LL}};

const cai_model_info *cai_model_info_by_id(const char *model_id) {
  size_t i;

  if (model_id == NULL) {
    return NULL;
  }
  for (i = 0U; cai_models[i].id != NULL; i++) {
    if (strcmp(cai_models[i].id, model_id) == 0) {
      return &cai_models[i];
    }
  }
  return NULL;
}

int cai_model_supports(const char *model_id, unsigned int capability) {
  const cai_model_info *info;

  info = cai_model_info_by_id(model_id);
  if (info == NULL) {
    return 0;
  }
  return (info->capabilities & capability) == capability;
}

long long cai_model_context_window_tokens(const char *model_id) {
  const cai_model_info *info;

  info = cai_model_info_by_id(model_id);
  return info != NULL ? info->context_window_tokens : 0LL;
}

long long cai_model_auto_compact_token_limit(const char *model_id) {
  const cai_model_info *info;

  info = cai_model_info_by_id(model_id);
  return info != NULL ? info->auto_compact_token_limit : 0LL;
}
