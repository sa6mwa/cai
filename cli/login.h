#ifndef CAI_CLI_LOGIN_H
#define CAI_CLI_LOGIN_H

#include <cai/auth.h>
#include <cai/blob_store.h>
#include <pslog.h>

typedef int (*cai_cli_login_stop_fn)(void *);
/* Write a complete callback response without changing SIGPIPE disposition.
 * Returns zero on success, or -1 with errno set on socket/format failure. */
int cai_cli_login_write_response(int fd,
                                 const cai_chatgpt_login_response *response);
/* Run browser login and persist ChatGPT auth in the supplied encrypted store.
 * stop_fd/stop interrupt callback socket waits. STOP returns zero; the caller
 * acknowledges the host control after the sockets and login are closed.
 */
int cai_cli_login(const cai_blob_store *storage, pslog_logger *logger,
                  int stop_fd, cai_cli_login_stop_fn stop, void *context);

#endif
