#ifndef CAI_CLI_LOGIN_H
#define CAI_CLI_LOGIN_H

#include <cai/blob_store.h>
#include <pslog.h>

/* Run browser login and persist ChatGPT auth in the supplied encrypted store.
 */
int cai_cli_login(const cai_blob_store *storage, pslog_logger *logger);

#endif
