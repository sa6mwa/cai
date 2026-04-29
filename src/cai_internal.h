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

struct cai_agent {
  cai_client *client;
  char *model;
  char *instructions;
  cai_tool_registry *tools;
};

typedef struct cai_session_text_input {
  char *role;
  char *text;
  char *image_url;
  char *detail;
  int is_image;
} cai_session_input;

struct cai_session {
  cai_agent *agent;
  char *previous_response_id;
  cai_session_input *inputs;
  size_t input_count;
  size_t input_capacity;
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
int cai_set_error_http(cai_error *error, int code, long http_status,
                       const char *message, const char *detail,
                       const char *server_code, const char *request_id);

int cai_resolve_api_key(const cai_allocator *allocator,
                        const char *explicit_key, char **out, cai_error *error);

struct cai_content_part {
  char *type;
  char *text;
  char *image_url;
  char *detail;
};

struct cai_input_message {
  int kind;
  char *role;
  lonejson_object_array content;
  char *call_id;
  char *output;
};

struct cai_function_tool {
  char *name;
  char *description;
  char *parameters_json;
  int strict;
};

struct cai_response_create_params {
  cai_allocator allocator;
  char *model;
  char *instructions;
  char *previous_response_id;
  lonejson_object_array input;
  lonejson_object_array tools;
};

struct cai_conversation_items_params {
  cai_allocator allocator;
  lonejson_object_array items;
};

typedef struct cai_response_tool_call {
  char *id;
  char *call_id;
  char *name;
  char *arguments;
} cai_response_tool_call;

struct cai_response {
  char *id;
  char *status;
  char *output_text;
  char *raw_json;
  cai_response_tool_call *tool_calls;
  size_t tool_call_count;
};

typedef struct cai_input_item {
  char *id;
  char *type;
  char *role;
} cai_input_item;

struct cai_input_item_list {
  char *object;
  char *first_id;
  char *last_id;
  char *raw_json;
  int has_more;
  cai_input_item *items;
  size_t count;
};

struct cai_conversation {
  char *id;
  char *object;
};

typedef struct cai_json_builder {
  char *data;
  size_t length;
  size_t capacity;
} cai_json_builder;

int cai_json_builder_lit(cai_json_builder *builder, const char *text,
                         cai_error *error);
int cai_json_builder_string(cai_json_builder *builder, const char *value,
                            cai_error *error);
int cai_json_builder_field_string(cai_json_builder *builder, const char *name,
                                  const char *value, int *need_comma,
                                  cai_error *error);
int cai_serialize_input_messages_json(cai_json_builder *builder,
                                      const char *field_name,
                                      const lonejson_object_array *messages,
                                      cai_error *error);
int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error);
int cai_response_parse_json(const char *json, cai_response **out,
                            cai_error *error);
int cai_input_item_list_parse_json(const char *json, cai_input_item_list **out,
                                   cai_error *error);
int cai_build_url(const cai_allocator *allocator, const char *base_url,
                  const char *path, char **out, cai_error *error);
int cai_append_list_query_params(const cai_allocator *allocator, char **path,
                                 const cai_list_params *params,
                                 cai_error *error);
int cai_http_json_request(cai_client *client, const char *method,
                          const char *path, const char *request_json,
                          char **out_json, long *out_http_status,
                          char **out_request_id, cai_error *error);
int cai_set_openai_error(cai_error *error, long http_status, const char *body,
                         const char *request_id);
int cai_conversation_parse_json(const char *json, cai_conversation **out,
                                cai_error *error);

#endif
