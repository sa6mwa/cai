#ifndef CAI_CHROME_PROTOCOL_H
#define CAI_CHROME_PROTOCOL_H
#include "cai_internal.h"
#include <cai/chrome.h>
#include <pslog.h>
#include <stdint.h>
#define CAI_CHROME_REQUEST_LIMIT (1024U * 1024U)
#define CAI_CHROME_RESPONSE_LIMIT (16U * 1024U * 1024U)
/* A mapped, bounded JSON-RPC frame. The values preserve JSON null/results. */
typedef struct cai_chrome_rpc_error {
  int has_code;
  int64_t code;
  const char *message;
} cai_chrome_rpc_error;
#define CAI_CHROME_JSON_FIELD(type, member, key)                               \
  {key,                                                                        \
   LONEJSON__KEY_LEN(key),                                                     \
   LONEJSON__KEY_FIRST(key),                                                   \
   LONEJSON__KEY_LAST(key),                                                    \
   offsetof(type, member),                                                     \
   LONEJSON_FIELD_KIND_JSON_VALUE,                                             \
   LONEJSON_STORAGE_FIXED,                                                     \
   LONEJSON_OVERFLOW_FAIL,                                                     \
   LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE,                                 \
   0U,                                                                         \
   0U,                                                                         \
   NULL,                                                                       \
   NULL,                                                                       \
   0U,                                                                         \
   LONEJSON_SPOOL_CLASS_DEFAULT}
typedef struct cai_chrome_rpc {
  int has_id, has_params, has_result, has_error;
  const char *jsonrpc;
  lonejson_json_value id;
  const char *method;
  lonejson_json_value params;
  lonejson_json_value result;
  cai_chrome_rpc_error error;
} cai_chrome_rpc;
extern const lonejson_map cai_chrome_rpc_map;
extern const lonejson_map cai_chrome_request_map;
extern const lonejson_map cai_chrome_success_map;
extern const lonejson_map cai_chrome_failure_map;
int cai_chrome_parse(const char *json, cai_chrome_rpc *rpc, cai_error *error);
char *cai_chrome_serialize(const lonejson_map *map, const void *doc,
                           cai_error *error);
char *cai_chrome_value(const lonejson_json_value *value, cai_error *error);
int cai_chrome_value_buffer(lonejson_json_value *value, const char *json,
                            cai_error *error);
long long cai_chrome_now_ms(void);
int cai_chrome_private_directory(const char *path, cai_error *error);
int cai_chrome_connect_socket(const char *path, int *out, cai_error *error);
/* Tool state lives in the connection, borrowed by a registry. */
struct cai_chrome {
  int fd;
  long timeout_ms;
  size_t limit;
  unsigned long sequence;
  pslog_logger *logger;
  char *session_id;
  char *turn_id;
  char *pending_image;
  int used;
  int cleanup;
  int lazy;
  char *socket_path;
  char *socket_directory;
};
#endif
