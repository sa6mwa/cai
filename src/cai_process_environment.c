#include "cai_process_environment.h"
#include "cai_internal.h"
#include <string.h>
extern char **environ;

void cai_process_environment_free(char **environment) {
  size_t i;
  if (environment == NULL)
    return;
  for (i = 0U; environment[i] != NULL; i++)
    cai_free_mem(NULL, environment[i]);
  cai_free_mem(NULL, environment);
}
static int preserved(const char *entry) {
  return strncmp(entry, "HOME=", 5U) == 0 || strncmp(entry, "PATH=", 5U) == 0 ||
         strncmp(entry, "XDG_", 4U) == 0;
}
int cai_process_environment_new(int yolo, const char *const *defaults,
                                char ***out, cai_error *error) {
  size_t count = 0U, extra = 0U, i, j, used = 0U;
  char **copy;
  *out = NULL;
  if (environ != NULL)
    while (environ[count] != NULL)
      count++;
  if (!yolo && defaults != NULL)
    while (defaults[extra] != NULL)
      extra++;
  if (count > SIZE_MAX / sizeof(*copy) - extra - 1U)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "command environment is too large");
  copy = cai_alloc(NULL, (count + extra + 1U) * sizeof(*copy));
  if (copy == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "copy command environment");
  memset(copy, 0, (count + extra + 1U) * sizeof(*copy));
  for (i = 0U; i < count; i++) {
    if (!yolo && !preserved(environ[i]))
      continue;
    copy[used] = cai_strdup(NULL, environ[i]);
    if (copy[used] == NULL)
      goto failed;
    used++;
  }
  for (i = 0U; i < extra; i++) {
    const char *equal = strchr(defaults[i], '=');
    size_t length = equal != NULL ? (size_t)(equal - defaults[i]) + 1U : 0U;
    for (j = 0U; j < used; j++)
      if (length != 0U && strncmp(copy[j], defaults[i], length) == 0)
        break;
    if (j != used)
      continue;
    copy[used] = cai_strdup(NULL, defaults[i]);
    if (copy[used] == NULL)
      goto failed;
    used++;
  }
  *out = copy;
  return CAI_OK;
failed:
  cai_process_environment_free(copy);
  return cai_set_error(error, CAI_ERR_NOMEM, "copy command environment");
}
