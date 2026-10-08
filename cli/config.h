#ifndef CAI_CLI_CONFIG_H
#define CAI_CLI_CONFIG_H
#include "options.h"
#include <cai/config.h>
/* Help/version bypass file effects. Owned configuration outlives clients and
 * runtimes borrowing its strings. Return 1=start, 0=help/version, -1=error. */
int cai_cli_configure(int argc, char *const *argv, cai_cli_options *options,
                      cai_config **configuration, cai_error *error);
#endif
