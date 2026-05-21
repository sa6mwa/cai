#ifndef CAI_TOOLS_REVGEO_H
#define CAI_TOOLS_REVGEO_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default public reverse-geocoding provider base URL. */
#define CAI_REVGEO_DEFAULT_BASE_URL "https://nominatim.openstreetmap.org"
/** Default reverse-geocoding endpoint path. */
#define CAI_REVGEO_DEFAULT_REVERSE_PATH "/reverse"

/** Configuration for the reverse-geocoding tool preset. */
typedef struct cai_revgeo_tool_config {
  /** Optional override for the model-facing tool name. */
  const char *name;
  /** Optional override for the model-facing tool description. */
  const char *description;
  /** Provider base URL; defaults to CAI_REVGEO_DEFAULT_BASE_URL. */
  const char *base_url;
  /** Reverse endpoint path; defaults to CAI_REVGEO_DEFAULT_REVERSE_PATH. */
  const char *reverse_path;
  /** HTTP User-Agent value sent to the provider. */
  const char *user_agent;
  /** Optional language preference sent to the provider. */
  const char *language;
  /** Optional reverse-geocoding zoom/detail level; zero selects default. */
  int zoom;
  /** HTTP timeout in milliseconds; zero selects default. */
  long timeout_ms;
  /** In-memory bytes retained before provider responses spill to disk. */
  size_t response_memory_limit;
  /** Maximum provider response bytes accepted. */
  size_t response_max_bytes;
  /** Optional directory for response spool spill files. */
  const char *response_spool_dir;
} cai_revgeo_tool_config;

/** Register the reverse-geocoding preset on a registry. */
int cai_tool_registry_register_revgeo_tool(
    cai_tool_registry *registry, const cai_revgeo_tool_config *config,
    cai_error *error);
/** Register the reverse-geocoding preset on an agent. */
int cai_agent_register_revgeo_tool(cai_agent *agent,
                                   const cai_revgeo_tool_config *config,
                                   cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
