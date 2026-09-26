#ifndef CAI_CLI_SESSION_COMMANDS_H
#define CAI_CLI_SESSION_COMMANDS_H

#include "pouch.h"
#include <cai/agent_runtime.h>

/* Plain session rows, numbered for resume; width is measured in terminal cells.
 */
int cai_cli_sessions_print(cai_sink *destination,
                           const cai_cli_session *sessions, size_t count,
                           int numbered, int width, cai_error *error);
int cai_cli_session_export(cai_cli_pouch *pouch, const char *id,
                           const char *export_directory,
                           cai_agent_runtime *active_runtime,
                           cai_sink *destination, cai_error *error);

#endif
