#ifndef CAI_CLI_POUCH_H
#define CAI_CLI_POUCH_H

#include "options.h"
#include "pouch_stream.h"
#include <cai/agent_runtime.h>
#include <cai/blob_store.h>
#include <cai/session_store.h>
#include <cai/tools/todo.h>
#include <limits.h>
#include <pthread.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cai_cli_session {
  char id[CAI_AGENT_SESSION_ID_MAX];
  char workspace[PATH_MAX];
  char first_prompt[512];
  char history_backend[16];
  lonejson_uint64 checkpoint_position;
  lonejson_uint64 checkpoint_ns;
  lonejson_uint64 applied_sequence;
  lonejson_int64 published;
} cai_cli_session;

typedef struct cai_cli_pouch {
  lc_client *sessions;
  struct cai_cli_pouch_client *connection;
  /* Anchors keep active partitions open; operation receivers are owned. */
  lc_collection *history_anchors[2];
  pthread_mutex_t history_lock;
  int history_lock_initialized;
  char program_name[33], queue_prefix[33];
  char sessions_namespace[129], auth_namespace[129], scopes_namespace[129];
  char todo_namespace[129], messages_namespace[129];
  cai_agent_session_store store;
  cai_blob_store credentials;
  pslog_logger *logger;
  /* Borrowed, synchronous replay observer; installed only during CLI open. */
  int (*replay_begin)(void *, const char *, cai_error *);
  cai_agent_session_event_fn replay_event;
  void *replay_context;
  const char *replay_scope;
  pthread_t owner_thread;
  char state_directory[PATH_MAX];
  char config_directory[PATH_MAX];
  char key_path[PATH_MAX];
  char root_directory[PATH_MAX];
  struct cai_cli_session_guard *use_guard;
  int capture_session;
} cai_cli_pouch;

int cai_cli_pouch_open_options(cai_cli_pouch *pouch,
                               const cai_cli_options *options,
                               pslog_logger *logger, cai_error *error);
int cai_cli_pouch_open(cai_cli_pouch *pouch, const char *endpoint,
                       const char *client_pem, pslog_logger *logger,
                       cai_error *error);
int cai_cli_pouch_open_at(cai_cli_pouch *pouch, const char *state_directory,
                          const char *endpoint, const char *client_pem,
                          pslog_logger *logger, cai_error *error);
void cai_cli_pouch_close(cai_cli_pouch *pouch);
/* Console tail only: bounded collection range, selection before rendering,
 * without invoking recovery or startup observers a second time. */
int cai_cli_pouch_replay(cai_cli_pouch *pouch, const char *scope,
                         const char *id, unsigned long long after,
                         unsigned long long through,
                         int (*select_event)(void *, unsigned long long),
                         cai_agent_session_event_fn visit, void *context,
                         cai_error *error);
int cai_cli_pouch_seed_auth(cai_cli_pouch *pouch, const char *path,
                            cai_error *error);
int cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                       cai_cli_session **out, size_t *count, cai_error *error);
int cai_cli_pouch_session(cai_cli_pouch *pouch, const char *id,
                          cai_cli_session *out, cai_error *error);
int cai_cli_pouch_publish(cai_cli_pouch *pouch, const char *id, int published,
                          cai_error *error);
/* Capture the root runtime's session identity during its synchronous open.
 * A shared filesystem lock stays held until end_use (after runtime close). */
void cai_cli_pouch_begin_use(cai_cli_pouch *pouch);
void cai_cli_pouch_end_use(cai_cli_pouch *pouch);
int cai_cli_pouch_begin_read(cai_cli_pouch *pouch, const char *id,
                             struct cai_cli_session_guard **guard,
                             cai_error *error);
void cai_cli_pouch_end_read(struct cai_cli_session_guard *guard);
int cai_cli_pouch_deletable(cai_cli_pouch *pouch, const char *id, int *eligible,
                            cai_error *error);
int cai_cli_pouch_delete(cai_cli_pouch *pouch, const char *workspace,
                         const char *id,
                         int (*cleanup)(void *, const char *, cai_error *),
                         void *context, cai_error *error);
int cai_cli_pouch_lock(cai_cli_pouch *pouch, const char *id, lc_lease **out,
                       cai_error *error);
int cai_cli_pouch_unlock(lc_lease *lease, int result, cai_error *error);
/* Global kanban document, encrypted in cai.todo. Context is the borrowed pouch.
 * Intermediate rewrites stay in encrypted attachments until transaction commit.
 */
const cai_todo_store_callbacks *cai_cli_pouch_todo_callbacks(void);
/* Stream JSONL containing the current complete checkpoint and all journal
 * events. */
int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id,
                         cai_agent_runtime *runtime, cai_sink *sink,
                         cai_error *error);
int cai_cli_pouch_import(cai_cli_pouch *pouch, const char *path,
                         const char *workspace, char *id, size_t capacity,
                         cai_error *error);

#endif
