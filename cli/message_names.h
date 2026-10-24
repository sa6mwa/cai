#ifndef CAI_CLI_MESSAGE_NAMES_H
#define CAI_CLI_MESSAGE_NAMES_H

#include <cai/cai.h>

/* Borrowed snapshot of persisted mailbox bindings. No listener is required. */
typedef struct cai_cli_message_name {
  const char *hostname, *host_id, *workspace, *name;
  int automatic;
} cai_cli_message_name;

/* Canonical absolute paths; real hostname is a leading component. Preserve
 * registered assignments and choose the fewest distinguishing ancestors.
 * Equal-size choices prefer ancestors nearest the basename. Never synthesize,
 * sanitize, truncate or abbreviate components. Output includes the configured
 * prefix. The caller holds the shared naming lease through the subsequent
 * claim. */
int cai_cli_message_name_select(const char *prefix,
                                const cai_cli_message_name *self,
                                const cai_cli_message_name *known, size_t count,
                                char out[129], cai_error *error);

#endif
