#ifndef CAI_CLI_SKILLS_H
#define CAI_CLI_SKILLS_H
#include "options.h"
#include <pslog.h>
/* Install one trusted, stable local package; no authentication/model calls. */
int cai_cli_install_skill(const cai_cli_options *options, pslog_logger *logger);
#endif
