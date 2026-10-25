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
 * sampling and must not enter a language runtime. CAI copies the callback
 * table; context remains borrowed until the owning agent or runtime closes.
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

typedef struct cai_skill_metadata cai_skill_metadata;
/** Owned, immutable skill identity with borrowed string getters. */
struct cai_skill_metadata {
  /** Borrow the parsed name until close. */
  const char *(*name)(const cai_skill_metadata *self);
  /** Borrow the normalized single-line catalog description until close. */
  const char *(*description)(const cai_skill_metadata *self);
  /** Destroy metadata and its owned strings. NULL is accepted. */
  void (*close)(cai_skill_metadata *self);
  /** Private implementation; do not access directly. */
  void *impl;
};
/** Parse YAML front matter (buffered, maximum 1 MiB), not the Markdown body.
 * The source is borrowed and consumed, with bounded read-ahead; reopen/reset
 * it before reading the full document. Quoted/folded/literal scalars and YAML
 * aliases are resolved. Name falls back to fallback_name's basename when
 * absent; description is required. Unknown metadata is ignored. Name is
 * limited to 64 bytes and description to 1024 bytes; catalog descriptions
 * collapse whitespace to a single line. No filesystem/environment effects. */
int cai_skill_metadata_open(cai_source *source, const char *fallback_name,
                            cai_skill_metadata **out, cai_error *error);
const char *cai_skill_metadata_name(const cai_skill_metadata *self);
const char *cai_skill_metadata_description(const cai_skill_metadata *self);
void cai_skill_metadata_close(cai_skill_metadata *self);

/** Initialize a zero-defaultable global skill configuration. */
void cai_skill_config_init(cai_skill_config *config);

#ifdef __cplusplus
}
#endif

#endif
