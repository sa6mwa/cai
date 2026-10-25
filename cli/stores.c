#include "stores.h"
#include "pouch_stream.h"
#include <stdio.h>
#include <string.h>

int cai_cli_stores_resolve(cai_config *config, cai_error *error) {
  size_t i;
  const char *resolved;
  char section[80];
  int rc;
  for (i = 0U; i < config->section_count(config); i++) {
    const char *name = config->section_at(config, i);
    if (strncmp(name, "store.", 6U) != 0)
      continue;
    if (strlen(name) >= sizeof(section))
      return cai_cli_error(error, CAI_ERR_INVALID, "store name is too long");
    strcpy(section, name);
    rc = config->resolve_path(config, section, "key_file", &resolved, error);
    if (rc == CAI_OK)
      rc =
          config->resolve_path(config, section, "client_pem", &resolved, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}

int cai_cli_store_options(cai_config *config, const cai_cli_options *defaults,
                          const char *name, cai_cli_options *out,
                          cai_error *error) {
  char section[80], message[160];
  const char *endpoint, *key, *pem;
  *out = *defaults;
  if (name != NULL && strcmp(name, "default") == 0)
    return CAI_OK;
  if (name == NULL || name[0] == '\0' || strlen(name) > 64U ||
      strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "0123456789_-") != strlen(name))
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid store name");
  snprintf(section, sizeof(section), "store.%s", name);
  endpoint = config->get(config, section, "endpoint");
  key = config->get(config, section, "key_file");
  pem = config->get(config, section, "client_pem");
  if (endpoint == NULL || endpoint[0] == '\0' ||
      (strncmp(endpoint, "pouch://", 8U) != 0 &&
       strncmp(endpoint, "https://", 8U) != 0 &&
       strncmp(endpoint, "http://", 7U) != 0)) {
    snprintf(message, sizeof(message),
             "[%s] requires a pouch:// or HTTP(S) endpoint", section);
    return cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  if (strncmp(endpoint, "pouch://", 8U) == 0 &&
      (key == NULL || key[0] == '\0')) {
    snprintf(message, sizeof(message),
             "[%s] requires key_file for its pouch root", section);
    return cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  out->lockd = endpoint;
  out->pouch_key_file = key != NULL && key[0] != '\0' ? key : NULL;
  out->lockd_client_pem = pem != NULL && pem[0] != '\0' ? pem : NULL;
  return CAI_OK;
}
