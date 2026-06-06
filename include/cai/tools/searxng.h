#ifndef CAI_TOOLS_SEARXNG_H
#define CAI_TOOLS_SEARXNG_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default local SearXNG base URL used by the search preset. */
#define CAI_SEARXNG_DEFAULT_BASE_URL "http://127.0.0.1:8888"
/** Default SearXNG search endpoint path. */
#define CAI_SEARXNG_DEFAULT_SEARCH_PATH "/search"

/** Configuration for registering the SearXNG web search preset tool. */
typedef struct cai_searxng_tool_config {
  /** Tool name exposed to the model, or NULL for the default name. */
  const char *name;
  /** Tool description exposed to the model, or NULL for the default. */
  const char *description;
  /** SearXNG base URL, or NULL for CAI_SEARXNG_DEFAULT_BASE_URL. */
  const char *base_url;
  /** Search path, or NULL for CAI_SEARXNG_DEFAULT_SEARCH_PATH. */
  const char *search_path;
  /** Optional SearXNG engine selector; NULL lets SearXNG decide. */
  const char *engine;
  /** Optional language selector sent to SearXNG. */
  const char *language;
  /** Request timeout in milliseconds; zero uses the preset default. */
  long timeout_ms;
  /** In-memory response spool limit before spilling to disk. */
  size_t response_memory_limit;
  /** Maximum accepted response body size in bytes; zero uses default. */
  size_t response_max_bytes;
  /** Optional directory for response spill files. */
  const char *response_spool_dir;
} cai_searxng_tool_config;

/** Register the SearXNG search preset in a standalone tool registry. */
int cai_tool_registry_register_searxng_tool(
    cai_tool_registry *registry, const cai_searxng_tool_config *config,
    cai_error *error);
/** Register the SearXNG search preset directly on an agent. */
int cai_agent_register_searxng_tool(cai_agent *agent,
                                    const cai_searxng_tool_config *config,
                                    cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
