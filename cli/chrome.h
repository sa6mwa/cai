#ifndef CAI_CLI_CHROME_H
#define CAI_CLI_CHROME_H
#include <cai/chrome.h>
#include <pslog.h>
/* No filesystem work outside an explicit setup/remove/native-host command. */
int cai_cli_chrome_directory(char *out, size_t capacity, int create,
                             cai_error *error);
int cai_cli_chrome_setup(const char *manifest, int remove, pslog_logger *logger,
                         cai_error *error);
/* Reject system registrations before publishing a user-level shadow. Paths
 * are explicit so offline tests never change system browser configuration. */
int cai_cli_chrome_check_system_registration(const char *const *paths,
                                             size_t count, cai_error *error);
int cai_cli_chrome_host(const char *origin, pslog_logger *logger,
                        cai_error *error);
#endif
