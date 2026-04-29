#include <cai/models.h>

#include <string.h>

static const cai_model_info cai_models[] = {
    {CAI_MODEL_GPT_5_4_NANO,
     CAI_MODEL_CAP_RESPONSES | CAI_MODEL_CAP_REALTIME |
         CAI_MODEL_CAP_STREAMING | CAI_MODEL_CAP_FUNCTION_CALLING |
         CAI_MODEL_CAP_STRUCTURED_OUTPUTS | CAI_MODEL_CAP_IMAGE_INPUT},
    {CAI_MODEL_GPT_5_4_MINI,
     CAI_MODEL_CAP_RESPONSES | CAI_MODEL_CAP_REALTIME |
         CAI_MODEL_CAP_STREAMING | CAI_MODEL_CAP_FUNCTION_CALLING |
         CAI_MODEL_CAP_STRUCTURED_OUTPUTS | CAI_MODEL_CAP_IMAGE_INPUT},
    {CAI_MODEL_GPT_5_4,
     CAI_MODEL_CAP_RESPONSES | CAI_MODEL_CAP_REALTIME |
         CAI_MODEL_CAP_STREAMING | CAI_MODEL_CAP_FUNCTION_CALLING |
         CAI_MODEL_CAP_STRUCTURED_OUTPUTS | CAI_MODEL_CAP_IMAGE_INPUT},
    {NULL, 0U}};

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
