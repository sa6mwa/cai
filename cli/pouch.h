#ifndef CAI_CLI_POUCH_H
#define CAI_CLI_POUCH_H

#include "pouch_stream.h"
#include <cai/blob_store.h>
#include <cai/session_store.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cai_cli_session {
  char id[CAI_AGENT_SESSION_ID_MAX];
  char workspace[PATH_MAX];
  char first_prompt[512];
  char checkpoint_name[96];
  lonejson_uint64 checkpoint_ns;
  lonejson_uint64 applied_sequence;
  lonejson_int64 published;
} cai_cli_session;

typedef struct cai_cli_pouch {
  lc_client *sessions;
  lc_client *auth;
  cai_agent_session_store store;
  cai_blob_store credentials;
  char state_directory[PATH_MAX];
  char key_path[PATH_MAX];
} cai_cli_pouch;

int cai_cli_pouch_open(cai_cli_pouch *pouch, const char *endpoint,
                       const char *client_pem, cai_error *error);
void cai_cli_pouch_close(cai_cli_pouch *pouch);
int cai_cli_pouch_seed_auth(cai_cli_pouch *pouch, const char *path,
                            cai_error *error);
int cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                       cai_cli_session **out, size_t *count, cai_error *error);
int cai_cli_pouch_session(cai_cli_pouch *pouch, const char *id,
                          cai_cli_session *out, cai_error *error);
int cai_cli_pouch_publish(cai_cli_pouch *pouch, const char *id, int published,
                          cai_error *error);
int cai_cli_pouch_lock(cai_cli_pouch *pouch, const char *id, lc_lease **out,
                       cai_error *error);
int cai_cli_pouch_unlock(lc_lease *lease, int result, cai_error *error);
/* Stream JSONL containing the current complete checkpoint and all journal
 * events. */
int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id, cai_sink *sink,
                         cai_error *error);
int cai_cli_pouch_import(cai_cli_pouch *pouch, const char *path,
                         const char *workspace, char *id, size_t capacity,
                         cai_error *error);

#endif
