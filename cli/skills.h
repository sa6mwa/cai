#ifndef CAI_CLI_SKILLS_H
#define CAI_CLI_SKILLS_H
#include "options.h"
#include <cai/agent_runtime.h>
#include <pslog.h>
/* Install one trusted, stable local package; no authentication/model calls. */
int cai_cli_install_skill(const cai_cli_options *options, pslog_logger *logger);
/* Stream the active runtime's catalog as a Markdown table; never rescan files.
 */
int cai_cli_skills_markdown(cai_agent_runtime *runtime, int enabled,
                            int (*write)(void *, const char *, size_t),
                            void *context, cai_error *error);
#endif
