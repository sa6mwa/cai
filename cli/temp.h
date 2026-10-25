#ifndef CAI_CLI_TEMP_H
#define CAI_CLI_TEMP_H
#include "options.h"
#include <cai/cai.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
typedef struct cai_cli_temp {
  int parent_fd, root_fd, cwd_fd;
  char root[PATH_MAX], workspace[PATH_MAX], private_dir[PATH_MAX];
  char state[PATH_MAX], cache[PATH_MAX], data[PATH_MAX], history[PATH_MAX];
  char clipboard[PATH_MAX], logs[PATH_MAX], key[PATH_MAX], exports[PATH_MAX];
} cai_cli_temp;
void cai_cli_temp_init(cai_cli_temp *temp);
int cai_cli_temp_open(cai_cli_temp *temp, cai_cli_options *options,
                      cai_error *error);
/* Call only after workers, stores and softline have closed. Never follows
 * workspace symlinks. EOF, interruption and host STOP mean No. */
int cai_cli_temp_close(cai_cli_temp *temp, int force, int ask, int stop_fd,
                       int (*stop)(void *), void *context, int *kept,
                       cai_error *error);
#endif
