/** @file cai/smith.h
 *  Codex-inspired Smith coding-agent preset.
 */
#ifndef CAI_SMITH_H
#define CAI_SMITH_H

#include <cai/cai.h>
#include <cai/tools/terminal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stable built-in coding-agent preset name. */
#define CAI_SMITH_PRESET "smith"
/** Stable name of the isolated Smith review preset. */
#define CAI_SMITH_REVIEW_PRESET "smith-review"
/** Smith's default model. */
#define CAI_SMITH_DEFAULT_MODEL CAI_MODEL_GPT_5_6_TERRA
/** Smith's default visible identity. */
#define CAI_SMITH_DEFAULT_IDENTITY "Cai Smith"
/** Version of the developer-instruction asset rendered by this preset. */
#define CAI_SMITH_PROMPT_VERSION "smith-5"

/** Tool capabilities selected by an agent preset.
 *
 * The runtime realizes every capability for which it has the required host
 * configuration. The direct cai_client_new_preset_agent constructors realize
 * the local file, terminal, and image-view capabilities; MCP, hosted image
 * generation, and goal state require a live cai_agent_runtime.
 */
/** Register the sandboxed UTF-8 file reader. */
#define CAI_AGENT_PRESET_TOOL_READ_FILE (1UL << 0)
/** Register the sandboxed file lister. */
#define CAI_AGENT_PRESET_TOOL_LIST_FILES (1UL << 1)
/** Register the native Codex-style apply_patch tool. */
#define CAI_AGENT_PRESET_TOOL_APPLY_PATCH (1UL << 2)
/** Register CAI's one-slot exec_command/write_stdin terminal pair. */
#define CAI_AGENT_PRESET_TOOL_TERMINAL (1UL << 3)
/** Register the local image viewer when the selected model supports images. */
#define CAI_AGENT_PRESET_TOOL_VIEW_IMAGE (1UL << 4)
/** Register durable goal lifecycle tools on a runtime session. */
#define CAI_AGENT_PRESET_TOOL_GOAL (1UL << 5)
/** Permit configured MCP client tools on an ordinary runtime. */
#define CAI_AGENT_PRESET_TOOL_MCP (1UL << 6)
/** Permit configured hosted image-generation on an ordinary runtime. */
#define CAI_AGENT_PRESET_TOOL_IMAGE_GENERATION (1UL << 7)

/** Declarative, host-defined agent profile for CAI's shared runtime. */
typedef struct cai_agent_preset {
  /** Stable non-empty host name persisted in session/export metadata. */
  const char *name;
  /** Stable non-empty prompt revision persisted with the session. */
  const char *prompt_version;
  /** Default visible identity when the runtime config does not override it. */
  const char *default_identity;
  /** Default model when the runtime config does not override it. */
  const char *default_model;
  /** Default reasoning effort when the runtime config does not override it. */
  const char *default_reasoning_effort;
  /** Default provider reasoning-summary mode; NULL means auto. */
  const char *default_reasoning_summary;
  /**
   * Full developer-instruction template for ordinary runs. The literal token
   * {{agent_identity}} is substituted with the selected identity. NULL is
   * reserved for the built-in Smith template.
   */
  const char *developer_instructions;
  /**
   * Full reviewer template. It uses the same identity token. NULL selects
   * Smith's built-in review rubric when supports_review is non-zero.
   */
  const char *review_developer_instructions;
  /** Bitwise OR of CAI_AGENT_PRESET_TOOL_* capabilities for ordinary runs. */
  unsigned long tool_capabilities;
  /**
   * Bitwise OR of CAI_AGENT_PRESET_TOOL_READ_FILE, _LIST_FILES, _TERMINAL,
   * and _VIEW_IMAGE for read-only review runs. Other capability bits fail
   * construction rather than granting a reviewer mutation authority.
   */
  unsigned long review_tool_capabilities;
  /** Non-zero enables isolated review children for this preset. */
  int supports_review;
} cai_agent_preset;

/**
 * Initialize a descriptor that exactly selects CAI's built-in Smith policy.
 * The resulting descriptor contains borrowed static strings and can be copied
 * or passed directly to a constructor/runtime open call.
 */
void cai_agent_preset_from_smith(cai_agent_preset *preset);

/** Generic per-runtime settings used with a cai_agent_preset. */
typedef struct cai_agent_preset_config {
  /** Canonical workspace root used to constrain local file tools. */
  const char *workspace_directory;
  /** Optional visible identity; NULL selects preset->default_identity. */
  const char *agent_identity;
  /** Optional model override; NULL selects preset->default_model. */
  const char *model;
  /** Optional reasoning effort; NULL selects the preset default. */
  const char *reasoning_effort;
  /** Optional reasoning-summary mode; NULL selects the preset default. */
  const char *reasoning_summary;
  /** Optional host-owned extension appended after the rendered template. */
  const char *developer_instructions_extension;
  /** In-memory bytes retained by each file reader before spill. */
  size_t file_content_memory_limit;
  /** Maximum bytes returned by read_file; zero selects its normal default. */
  size_t file_content_max_bytes;
  /** Optional spill directory for file-reader content. */
  const char *file_content_spool_dir;
  /** Optional terminal policy/limit override; root remains the workspace. */
  const cai_terminal_tool_config *terminal_tool_config;
  /** Non-zero disables the preset's one-slot terminal tools. */
  int disable_terminal;
} cai_agent_preset_config;

/** Initialize a zero-defaultable generic preset configuration. */
void cai_agent_preset_config_init(cai_agent_preset_config *config);

/** Configuration for a Smith agent profile. */
typedef cai_agent_preset_config cai_smith_config;

/** Initialize a zero-defaultable Smith configuration. */
void cai_smith_config_init(cai_smith_config *config);
/** Return the version of Smith's rendered developer-instruction asset. */
const char *cai_smith_prompt_version(void);
/**
 * Construct an ordinary cai agent configured as Smith.
 *
 * Smith always uses client-side history, retains local history, serializes
 * tool calls, and registers read_file, list_files, apply_patch, exec_command,
 * and write_stdin. Models with image-input capability also receive view_image.
 * Smith never registers the legacy synchronous command runner. client remains
 * borrowed and must outlive the returned agent; config is consumed during the
 * call and may be stack allocated.
 */
int cai_client_new_smith_agent(cai_client *client,
                               const cai_smith_config *config, cai_agent **out,
                               cai_error *error);

/**
 * Construct an ordinary agent from a host-defined preset.
 *
 * This lower-level constructor is useful when the host owns its own session
 * loop. Use cai_agent_runtime_open for persisted sessions, goals, MCP, hosted
 * tools, events, and review-child orchestration. client remains borrowed and
 * must outlive the returned agent; preset/config are consumed during the call.
 */
int cai_client_new_preset_agent(cai_client *client,
                                const cai_agent_preset *preset,
                                const cai_agent_preset_config *config,
                                cai_agent **out, cai_error *error);

/**
 * Construct an isolated Smith reviewer. The reviewer has a separate session,
 * uses the dedicated review rubric, and registers read_file, list_files,
 * exec_command, write_stdin, and view_image when the selected model supports
 * images. It never exposes patch, goal, MCP, or image-generation tools.
 * Terminal commands remain subject to the configured host terminal policy.
 * client remains borrowed and must outlive the returned agent; config is
 * consumed during the call and may be stack allocated.
 */
int cai_client_new_smith_review_agent(cai_client *client,
                                      const cai_smith_config *config,
                                      cai_agent **out, cai_error *error);

/**
 * Construct an isolated reviewer from a preset that supports review. client
 * remains borrowed and must outlive the returned agent; preset/config are
 * consumed during the call.
 */
int cai_client_new_preset_review_agent(cai_client *client,
                                       const cai_agent_preset *preset,
                                       const cai_agent_preset_config *config,
                                       cai_agent **out, cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
