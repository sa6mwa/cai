#ifndef CAI_CLI_STORES_H
#define CAI_CLI_STORES_H
#include "options.h"
#include <cai/config.h>

/* Resolve all named key paths before changing workspace. Configuration stays
 * in memory; no template or file is rewritten. */
int cai_cli_stores_resolve(cai_config *config, cai_error *error);
int cai_cli_store_options(cai_config *config, const cai_cli_options *defaults,
                          const char *name, cai_cli_options *out,
                          cai_error *error);
#endif
