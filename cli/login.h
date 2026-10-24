#ifndef CAI_CLI_LOGIN_H
#define CAI_CLI_LOGIN_H

#include <cai/blob_store.h>
#include <pslog.h>

typedef int (*cai_cli_login_stop_fn)(void *);
/* Run browser login and persist ChatGPT auth in the supplied encrypted store.
 * stop_fd/stop interrupt callback socket waits. STOP returns zero; the caller
 * acknowledges the host control after the sockets and login are closed.
 */
int cai_cli_login(const cai_blob_store *storage, pslog_logger *logger,
                  int stop_fd, cai_cli_login_stop_fn stop, void *context);

#endif
