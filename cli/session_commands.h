#ifndef CAI_CLI_SESSION_COMMANDS_H
#define CAI_CLI_SESSION_COMMANDS_H

#include "pouch.h"
#include <cai/agent_runtime.h>

/* Resolve an exact ID or a numbered current-directory listing entry. An exact
 * numeric ID takes precedence over its interpretation as a row number. */
int cai_cli_session_select(cai_cli_pouch *pouch, const char *workspace,
                           const char *choice,
                           char out[CAI_AGENT_SESSION_ID_MAX],
                           cai_error *error);

/* Plain session rows, numbered for resume; width is measured in terminal cells.
 */
int cai_cli_sessions_print(cai_sink *destination,
                           const cai_cli_session *sessions, size_t count,
                           int numbered, int width, cai_error *error);
/* One literal history row, clipped by display cells including its number. */
void cai_cli_history_row(char *out, size_t capacity,
                         const cai_agent_history_entry *entry, int width);
int cai_cli_session_select_delete(cai_cli_pouch *pouch, const char *workspace,
                                  const char *choice,
                                  char out[CAI_AGENT_SESSION_ID_MAX],
                                  cai_error *error);
int cai_cli_sessions_deletable(cai_cli_pouch *pouch, cai_cli_session *sessions,
                               size_t *count, cai_error *error);
int cai_cli_session_delete(cai_cli_pouch *pouch, cai_cli_pouch *messaging,
                           const cai_cli_options *options,
                           const char *workspace, const char *choice,
                           char id[CAI_AGENT_SESSION_ID_MAX], cai_error *error);
/* NULL basename selects paired ID files under export_directory. A named
 * .md/.jsonl selects one projection; any other name gets both suffixes.
 * Relative names use the selected workspace/current directory. */
int cai_cli_session_export(cai_cli_pouch *pouch, const char *id,
                           const char *export_directory, const char *basename,
                           cai_agent_runtime *active_runtime,
                           cai_sink *destination, cai_error *error);

#endif
