#ifndef CAI_TOOLS_PATCH_H
#define CAI_TOOLS_PATCH_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default registered name for CAI's native patch tool. */
#define CAI_PATCH_DEFAULT_TOOL_NAME "apply_patch"

/** Configuration for the native Codex-style patch tool. */
typedef struct cai_patch_tool_config {
  /** Optional model-facing tool-name override. */
  const char *name;
  /** Optional model-facing tool-description override. */
  const char *description;
  /** Required workspace root; every changed path must remain below it. */
  const char *root_path;
  /** Largest accepted patch body in bytes; zero uses the secure default. */
  size_t max_patch_bytes;
  /** Largest regular file read or written by a patch; zero uses the default. */
  size_t max_file_bytes;
} cai_patch_tool_config;

/** Apply a Codex-style patch within the configured workspace root. */
int cai_apply_patch(const cai_patch_tool_config *config, const char *patch,
                    cai_sink *result, cai_error *error);
/** Register `apply_patch` on a local tool registry. */
int cai_tool_registry_register_patch_tool(cai_tool_registry *registry,
                                          const cai_patch_tool_config *config,
                                          cai_error *error);
/** Register `apply_patch` directly on an agent. */
int cai_agent_register_patch_tool(cai_agent *agent,
                                  const cai_patch_tool_config *config,
                                  cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
