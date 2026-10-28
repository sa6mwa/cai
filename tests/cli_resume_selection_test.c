#include "../cli/session_commands.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static cai_cli_pouch store;
static int queries, fail_query, block_newest;
int __wrap_cai_cli_pouch_deletable(cai_cli_pouch *pouch, const char *id,
                                   int *eligible, cai_error *error);
int __wrap_cai_cli_pouch_deletable(cai_cli_pouch *pouch, const char *id,
                                   int *eligible, cai_error *error) {
  (void)error;
  assert(pouch == &store);
  *eligible =
      !block_newest || strcmp(id, block_newest == 1 ? "newest" : "2") != 0;
  return CAI_OK;
}
int __wrap_cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                              cai_cli_session **out, size_t *count,
                              cai_error *error);
int __wrap_cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                              cai_cli_session **out, size_t *count,
                              cai_error *error) {
  assert(pouch == &store && strcmp(scope, "/project") == 0);
  queries++;
  if (fail_query)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "listing failed");
  *out = calloc(3U, sizeof(**out));
  assert(*out != NULL);
  *count = 3U;
  strcpy((*out)[0].id, "newest");
  strcpy((*out)[1].id, "previous");
  strcpy((*out)[2].id, "2");
  return CAI_OK;
}
static void select_choice(const char *choice, const char *expected,
                          int status) {
  cai_error error;
  char id[CAI_AGENT_SESSION_ID_MAX];
  cai_error_init(&error);
  strcpy(id, "unchanged");
  assert(cai_cli_session_select(&store, "/project", choice, id, &error) ==
         status);
  assert(strcmp(id, expected != NULL ? expected : "unchanged") == 0);
  if (status != CAI_OK)
    assert(error.message != NULL);
  cai_error_cleanup(&error);
}
int main(void) {
  char long_id[CAI_AGENT_SESSION_ID_MAX + 1U];
  select_choice("saved-id", "saved-id", CAI_OK);
  assert(queries == 0);
  select_choice("1", "newest", CAI_OK);
  select_choice("2", "2", CAI_OK);
  select_choice("02", "previous", CAI_OK);
  select_choice("3", "2", CAI_OK);
  select_choice("0", NULL, CAI_ERR_INVALID);
  select_choice("4", NULL, CAI_ERR_INVALID);
  select_choice("9999999999999999999999999999999999", NULL, CAI_ERR_INVALID);
  select_choice("", NULL, CAI_ERR_INVALID);
  select_choice(NULL, NULL, CAI_ERR_INVALID);
  memset(long_id, 'x', sizeof(long_id) - 1U);
  long_id[sizeof(long_id) - 1U] = '\0';
  select_choice(long_id, NULL, CAI_ERR_INVALID);
  fail_query = 1;
  select_choice("1", NULL, CAI_ERR_TRANSPORT);
  fail_query = 0;
  block_newest = 1;
  {
    cai_error error;
    char id[CAI_AGENT_SESSION_ID_MAX];
    cai_error_init(&error);
    assert(cai_cli_session_select_delete(&store, "/project", "1", id, &error) ==
           CAI_OK);
    assert(strcmp(id, "previous") == 0);
    assert(cai_cli_session_select_delete(&store, "/project", "2", id, &error) ==
           CAI_OK);
    assert(strcmp(id, "2") == 0);
    block_newest = 2;
    assert(cai_cli_session_select_delete(&store, "/project", "2", id, &error) ==
           CAI_OK);
    assert(strcmp(id, "2") == 0);
    cai_error_cleanup(&error);
  }
  return 0;
}
