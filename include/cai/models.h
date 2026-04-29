#ifndef CAI_MODELS_H
#define CAI_MODELS_H

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_MODEL_GPT_5_4_NANO "gpt-5.4-nano"
#define CAI_MODEL_GPT_5_4_MINI "gpt-5.4-mini"
#define CAI_MODEL_GPT_5_4 "gpt-5.4"

#define CAI_MODEL_CAP_RESPONSES 0x0001u
#define CAI_MODEL_CAP_REALTIME 0x0002u
#define CAI_MODEL_CAP_STREAMING 0x0004u
#define CAI_MODEL_CAP_FUNCTION_CALLING 0x0008u
#define CAI_MODEL_CAP_STRUCTURED_OUTPUTS 0x0010u
#define CAI_MODEL_CAP_IMAGE_INPUT 0x0020u
#define CAI_MODEL_CAP_AUDIO_INPUT 0x0040u
#define CAI_MODEL_CAP_AUDIO_OUTPUT 0x0080u

typedef struct cai_model_info {
  const char *id;
  unsigned int capabilities;
} cai_model_info;

const cai_model_info *cai_model_info_by_id(const char *model_id);
int cai_model_supports(const char *model_id, unsigned int capability);

#ifdef __cplusplus
}
#endif

#endif
