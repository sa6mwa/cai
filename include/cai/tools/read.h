#ifndef CAI_TOOLS_READ_H
#define CAI_TOOLS_READ_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default registered name for the UTF-8 text file reader preset. */
#define CAI_READ_DEFAULT_TOOL_NAME "read_file"
/** Default registered name for the sandboxed file listing preset. */
#define CAI_LIST_FILES_DEFAULT_TOOL_NAME "list_files"

/** Shared configuration for the `read_file` and `list_files` presets. */
typedef struct cai_read_tool_config {
  /** Optional override for the model-facing tool name. */
  const char *name;
  /** Optional override for the model-facing tool description. */
  const char *description;
  /** Required sandbox root; all resolved paths must remain under this path. */
  const char *root_path;
  /** Optional default working directory inside `root_path`. */
  const char *default_workdir;
  /** In-memory bytes retained before read content spills to disk. */
  size_t content_memory_limit;
  /** Maximum bytes returned by `read_file` before truncation. */
  size_t content_max_bytes;
  /** Optional directory for read content spool spill files. */
  const char *content_spool_dir;
} cai_read_tool_config;

/** Register the UTF-8 text-only `read_file` preset on a registry. */
int cai_tool_registry_register_read_tool(cai_tool_registry *registry,
                                         const cai_read_tool_config *config,
                                         cai_error *error);
/** Register the UTF-8 text-only `read_file` preset on an agent. */
int cai_agent_register_read_tool(cai_agent *agent,
                                 const cai_read_tool_config *config,
                                 cai_error *error);
/** Register the sandboxed `list_files` preset on a registry. */
int cai_tool_registry_register_list_files_tool(cai_tool_registry *registry,
                                               const cai_read_tool_config *config,
                                               cai_error *error);
/** Register the sandboxed `list_files` preset on an agent. */
int cai_agent_register_list_files_tool(cai_agent *agent,
                                       const cai_read_tool_config *config,
                                       cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
