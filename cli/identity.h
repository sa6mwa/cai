#ifndef CAI_CLI_IDENTITY_H
#define CAI_CLI_IDENTITY_H
#include <cai/app.h>

/* Owned application strings copied by the C receiver. */
/* Keep the X-macro entries separate; clang-format otherwise merges them on
 * successive runs. */
/* clang-format off */
#define CAI_APP_STRINGS(X) \
  X(program_name) \
  X(invocation_name) \
  X(environment_prefix) \
  X(namespace_prefix) \
  X(queue_prefix) \
  X(home_directory) \
  X(state_directory) \
  X(cache_directory) \
  X(data_directory) \
  X(runtime_directory) \
  X(clipboard_directory) \
  X(log_directory) \
  X(pouch_key_file) \
  X(banner) \
  X(config_file) \
  X(config_directory) \
  X(introduction_markdown)
/* clang-format on */

int cai_cli_identity_valid(const char *name);
int cai_cli_environment_prefix(const cai_app_config *app, char out[65],
                               cai_error *error);
int cai_cli_xdg_path(char *out, size_t size, const char *variable,
                     const char *fallback, const char *name, cai_error *error);
int cai_cli_private_directory(const char *path, cai_error *error);
/* Remove one caller-owned child tree without following symlinks. Missing is
 * success; errors use errno. parent is a borrowed directory descriptor. */
int cai_cli_remove_at(int parent, const char *name);
#endif
