#ifndef CAI_TOOLS_SEARXNG_H
#define CAI_TOOLS_SEARXNG_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_SEARXNG_DEFAULT_BASE_URL "http://127.0.0.1:8888"
#define CAI_SEARXNG_DEFAULT_SEARCH_PATH "/search"

typedef struct cai_searxng_tool_config {
  const char *name;
  const char *description;
  const char *base_url;
  const char *search_path;
  const char *engine;
  const char *language;
  long timeout_ms;
  size_t response_memory_limit;
  size_t response_max_bytes;
  const char *response_spool_dir;
} cai_searxng_tool_config;

int cai_agent_register_searxng_tool(cai_agent *agent,
                                    const cai_searxng_tool_config *config,
                                    cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
