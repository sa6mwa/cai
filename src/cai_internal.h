#ifndef CAI_INTERNAL_H
#define CAI_INTERNAL_H

#include <cai/cai.h>

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

#endif
