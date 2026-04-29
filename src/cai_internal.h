#ifndef CAI_INTERNAL_H
#define CAI_INTERNAL_H

#include <cai/cai.h>

#include <lonejson.h>
#include <stddef.h>

#define CAI_DEFAULT_BASE_URL "https://api.openai.com/v1"
#define CAI_DEFAULT_JSON_RESPONSE_LIMIT (1024UL * 1024UL)

struct cai_client {
  cai_allocator allocator;
  char *api_key;
  char *base_url;
  char *organization_id;
  char *project_id;
  long timeout_ms;
  int prefer_http_2;
  int insecure_skip_verify;
  size_t json_response_limit_bytes;
  struct pslog_logger *logger;
};

void *cai_alloc(const cai_allocator *allocator, size_t size);
void *cai_realloc_mem(const cai_allocator *allocator, void *ptr, size_t size);
void cai_free_mem(const cai_allocator *allocator, void *ptr);
char *cai_strdup(const cai_allocator *allocator, const char *value);
char *cai_strndup(const cai_allocator *allocator, const char *value,
                  size_t length);

int cai_set_error(cai_error *error, int code, const char *message);
int cai_set_error_detail(cai_error *error, int code, const char *message,
                         const char *detail);

int cai_resolve_api_key(const cai_allocator *allocator,
                        const char *explicit_key, char **out, cai_error *error);

struct cai_content_part {
  char *type;
  char *text;
  char *image_url;
  char *detail;
};

struct cai_input_message {
  char *role;
  lonejson_object_array content;
};

struct cai_response_create_params {
  cai_allocator allocator;
  char *model;
  char *instructions;
  char *previous_response_id;
  lonejson_object_array input;
};

struct cai_response {
  char *id;
  char *status;
  char *output_text;
};

int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error);
int cai_response_parse_json(const char *json, cai_response **out,
                            cai_error *error);

#endif
