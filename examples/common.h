#ifndef CAI_EXAMPLES_COMMON_H
#define CAI_EXAMPLES_COMMON_H

#include <cai/cai.h>

static int cai_example_load_dotenv_api_key(cai_client_config *config,
                                           char **api_key, cai_error *error) {
  int rc;

  if (api_key == NULL || config == NULL) {
    return CAI_ERR_INVALID;
  }
  *api_key = NULL;
  rc = cai_load_dotenv_api_key(CAI_DEFAULT_DOTENV_PATH, config->api_key_env,
                               api_key, error);
  if (rc == CAI_ERR_CANCELLED) {
    cai_error_cleanup(error);
    cai_error_init(error);
    return CAI_OK;
  }
  if (rc != CAI_OK) {
    return rc;
  }
  config->api_key = *api_key;
  return CAI_OK;
}

#endif
