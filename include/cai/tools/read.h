#ifndef CAI_TOOLS_READ_H
#define CAI_TOOLS_READ_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_READ_DEFAULT_TOOL_NAME "read_file"
#define CAI_LIST_FILES_DEFAULT_TOOL_NAME "list_files"

typedef struct cai_read_tool_config {
  const char *name;
  const char *description;
  const char *root_path;
  const char *default_workdir;
  size_t content_memory_limit;
  size_t content_max_bytes;
  const char *content_spool_dir;
} cai_read_tool_config;

int cai_tool_registry_register_read_tool(cai_tool_registry *registry,
                                         const cai_read_tool_config *config,
                                         cai_error *error);
int cai_agent_register_read_tool(cai_agent *agent,
                                 const cai_read_tool_config *config,
                                 cai_error *error);
int cai_tool_registry_register_list_files_tool(cai_tool_registry *registry,
                                               const cai_read_tool_config *config,
                                               cai_error *error);
int cai_agent_register_list_files_tool(cai_agent *agent,
                                       const cai_read_tool_config *config,
                                       cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
