/** @file cai/smith.h
 *  Codex-inspired Smith coding-agent preset.
 */
#ifndef CAI_SMITH_H
#define CAI_SMITH_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stable name of the first CAI coding-agent preset. */
#define CAI_SMITH_PRESET "smith"
/** Smith's default model. */
#define CAI_SMITH_DEFAULT_MODEL CAI_MODEL_GPT_5_6_TERRA
/** Smith's default visible identity. */
#define CAI_SMITH_DEFAULT_IDENTITY "Cai Smith"
/** Version of the developer-instruction asset rendered by this preset. */
#define CAI_SMITH_PROMPT_VERSION "smith-1"

/** Configuration for a Smith agent profile. */
typedef struct cai_smith_config {
  /** Canonical workspace root used to constrain Smith's file tools. */
  const char *workspace_directory;
  /** Optional display identity; NULL selects CAI_SMITH_DEFAULT_IDENTITY. */
  const char *agent_identity;
  /** Optional model override; NULL selects CAI_SMITH_DEFAULT_MODEL. */
  const char *model;
  /** Optional reasoning effort; NULL selects medium. */
  const char *reasoning_effort;
  /** Optional developer-instruction extension owned by the embedding host. */
  const char *developer_instructions_extension;
  /** In-memory bytes retained by each file reader before spill. */
  size_t file_content_memory_limit;
  /** Maximum bytes returned by read_file; zero selects its normal default. */
  size_t file_content_max_bytes;
  /** Optional spill directory for file-reader content. */
  const char *file_content_spool_dir;
} cai_smith_config;

/** Initialize a zero-defaultable Smith configuration. */
void cai_smith_config_init(cai_smith_config *config);
/** Return the version of Smith's rendered developer-instruction asset. */
const char *cai_smith_prompt_version(void);
/**
 * Construct an ordinary cai agent configured as Smith.
 *
 * Smith always uses client-side history, retains local history, serializes
 * tool calls, and registers only read_file and list_files at this stage. It
 * intentionally does not register exec_command or another command runner.
 */
int cai_client_new_smith_agent(cai_client *client,
                               const cai_smith_config *config, cai_agent **out,
                               cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
