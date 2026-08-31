/** @file cai/skills.h
 *  Global agent-skill discovery and constrained resource access.
 */
#ifndef CAI_SKILLS_H
#define CAI_SKILLS_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Model-facing name of CAI's constrained skill resource reader. */
#define CAI_SKILL_READ_TOOL_NAME "read_skill"

/** Maximum bytes in an opaque provider skill identifier, excluding NUL. */
#define CAI_SKILL_ID_MAX_BYTES 1024U

/**
 * Visit one opaque skill package identifier during provider enumeration.
 * Identifiers must be non-empty and no longer than CAI_SKILL_ID_MAX_BYTES;
 * CAI otherwise preserves their contents without path interpretation.
 */
typedef int (*cai_skill_provider_visit_fn)(void *visit_context,
                                           const char *skill_id,
                                           cai_error *error);

/**
 * Read-only source for globally configured agent skills.
 *
 * list calls visit once for each available opaque package identifier. read
 * returns a newly owned source for a package-relative resource; resource NULL
 * means the package's SKILL.md. Callbacks may run while an agent worker is
 * sampling and must not enter a language runtime. CAI borrows the provider and
 * context until the owning preset agent or runtime closes.
 */
typedef struct cai_skill_provider {
  int (*list)(void *context, cai_skill_provider_visit_fn visit,
              void *visit_context, cai_error *error);
  int (*read)(void *context, const char *skill_id, const char *resource,
              cai_source **out, cai_error *error);
  void *context;
} cai_skill_provider;

/**
 * Optional global skills source for an agent preset or runtime.
 *
 * skills_directory selects one filesystem-backed root. NULL derives
 * <agent_config_directory>/skills, or $XDG_CONFIG_HOME/cai/skills (falling
 * back to $HOME/.config/cai/skills). skill_provider replaces filesystem
 * discovery; it is mutually exclusive with skills_directory. CAI never
 * discovers workspace or ancestor skill roots.
 */
typedef struct cai_skill_config {
  const char *skills_directory;
  const cai_skill_provider *skill_provider;
  /** Optional non-fatal discovery diagnostic receiver. */
  void (*warning_callback)(void *context, const char *message);
  /** Context passed to warning_callback. */
  void *warning_context;
} cai_skill_config;

/** Initialize a zero-defaultable global skill configuration. */
void cai_skill_config_init(cai_skill_config *config);

#ifdef __cplusplus
}
#endif

#endif
