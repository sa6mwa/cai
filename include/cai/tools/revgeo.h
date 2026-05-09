#ifndef CAI_TOOLS_REVGEO_H
#define CAI_TOOLS_REVGEO_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_REVGEO_DEFAULT_BASE_URL "https://nominatim.openstreetmap.org"
#define CAI_REVGEO_DEFAULT_REVERSE_PATH "/reverse"

typedef struct cai_revgeo_tool_config {
  const char *name;
  const char *description;
  const char *base_url;
  const char *reverse_path;
  const char *user_agent;
  const char *language;
  int zoom;
  long timeout_ms;
  size_t response_memory_limit;
  size_t response_max_bytes;
  const char *response_spool_dir;
} cai_revgeo_tool_config;

int cai_tool_registry_register_revgeo_tool(
    cai_tool_registry *registry, const cai_revgeo_tool_config *config,
    cai_error *error);
int cai_agent_register_revgeo_tool(cai_agent *agent,
                                   const cai_revgeo_tool_config *config,
                                   cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
