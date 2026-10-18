#ifndef CAI_CLI_RUNTIME_OPTIONS_H
#define CAI_CLI_RUNTIME_OPTIONS_H
#include "options.h"
#include <cai/agent_runtime.h>

/* Apply resolved CLI options to an initialized runtime config. Host I/O,
 * stores and callbacks are supplied by main. All inputs remain borrowed. */
void cai_cli_apply_runtime_options(const cai_cli_options *options,
                                   cai_agent_runtime_config *config,
                                   cai_agent_preset *preset,
                                   cai_skill_config *skills);
#endif
