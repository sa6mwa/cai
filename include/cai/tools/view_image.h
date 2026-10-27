/** @file cai/tools/view_image.h
 *  Sandboxed local image viewer for multimodal CAI agents.
 */
#ifndef CAI_TOOLS_VIEW_IMAGE_H
#define CAI_TOOLS_VIEW_IMAGE_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default model-facing name for the local image viewer. */
#define CAI_VIEW_IMAGE_DEFAULT_TOOL_NAME "view_image"

/** Configuration for a sandboxed local image viewer. */
typedef struct cai_view_image_tool_config {
  /** Optional model-facing tool-name override. */
  const char *name;
  /** Optional model-facing tool-description override. */
  const char *description;
  /** Required workspace base; confines image paths unless yolo is set. */
  const char *root_path;
  /** Optional default working directory inside root_path; must be a directory.
   */
  const char *default_workdir;
  /** Largest accepted image in bytes; zero selects the secure default. */
  size_t max_image_bytes;
  /** Optional additional image-only root; must be an existing directory.
   * Both resolved and opened paths are checked against these two roots.
   * Relative paths still resolve against default_workdir inside root_path.
   */
  const char *additional_root_path;
  /** Non-zero permits ordinary host image paths, including hard links.
   * Relative paths still use default_workdir (or root_path). */
  int yolo;
} cai_view_image_tool_config;

/**
 * Register view_image on a local registry. The tool validates a bounded local
 * image then attaches it as typed input to the next compatible model request;
 * it does not place base64 image bytes in normal tool output.
 */
int cai_tool_registry_register_view_image_tool(
    cai_tool_registry *registry, const cai_view_image_tool_config *config,
    cai_error *error);
/** Register view_image directly on an agent with the same registration rules.
 */
int cai_agent_register_view_image_tool(cai_agent *agent,
                                       const cai_view_image_tool_config *config,
                                       cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
