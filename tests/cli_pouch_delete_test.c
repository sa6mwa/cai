#define _POSIX_C_SOURCE 200809L
#include "../cli/identity.h"
#include "../cli/pouch.h"
#include "../cli/session_commands.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t read_state(void *context, void *bytes, size_t capacity,
                         cai_error *error) {
  size_t *offset = context, count = 2U - *offset;
  (void)error;
  if (count > capacity)
    count = capacity;
  memcpy(bytes, "{}" + *offset, count);
  *offset += count;
  return count;
}
static void save(cai_cli_pouch *pouch, const char *scope, const char *id,
                 cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  size_t offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = &offset;
  callbacks.read = read_state;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(pouch->store.commit(pouch, scope, id, source, 1U, error) == CAI_OK);
  cai_source_close(source);
}
static int collection(void *context, const lc_collection_info *info,
                      lc_error *error) {
  (void)info;
  (void)error;
  (*(int *)context)++;
  return LC_OK;
}
static int collections(cai_cli_pouch *pouch) {
  lc_error error;
  int count = 0;
  lc_error_init(&error);
  assert(lc_client_list_collections(pouch->sessions, pouch->sessions_namespace,
                                    collection, &count, &error) == LC_OK);
  lc_error_cleanup(&error);
  return count;
}
typedef struct cleanup_state {
  int fail, calls, seen;
} cleanup_state;
static int cleanup(void *context, const char *id, cai_error *error) {
  cleanup_state *state = context;
  state->calls++;
  if (state->fail)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "fixture cleanup failed");
  if (strcmp(id, "parent") == 0)
    state->seen |= 1;
  else if (strcmp(id, "worker-child") == 0)
    state->seen |= 2;
  else if (strcmp(id, "review-child") == 0)
    state->seen |= 4;
  else if (strcmp(id, "leaf-child") == 0)
    state->seen |= 8;
  else
    assert(0);
  return CAI_OK;
}

/* A valid JSON metadata document with an incorrect object identity must not
 * redirect deletion to a different conversation. */
static const lonejson_field identity_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, id, "id",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, workspace, "workspace",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, history_backend,
                                "history_backend", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(cai_cli_session, published, "published")};
LONEJSON_MAP_DEFINE(identity_map, cai_cli_session, identity_fields);
static void metadata_identity(cai_cli_pouch *pouch, const char *key,
                              const char *scope, const char *id) {
  cai_cli_session doc;
  lc_acquire_req request;
  lc_lease *lease = NULL;
  lc_error local;
  memset(&doc, 0, sizeof(doc));
  strcpy(doc.id, id);
  strcpy(doc.workspace, scope);
  strcpy(doc.history_backend, "collections");
  doc.published = 1;
  lc_error_init(&local);
  lc_acquire_req_init(&request);
  request.ns = pouch->sessions_namespace;
  request.key = key;
  request.owner = "fixture";
  request.ttl_seconds = 30;
  assert(lc_acquire(pouch->sessions, &request, &lease, &local) == LC_OK);
  assert(lc_lease_save(lease, &identity_map, &doc, NULL, &local) == LC_OK);
  assert(lc_lease_release(lease, NULL, &local) == LC_OK);
  lc_error_cleanup(&local);
}

typedef struct pending_fixture {
  char kind[16], pending_for[32];
} pending_fixture;
static const lonejson_field pending_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(pending_fixture, kind, "kind",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(pending_fixture, pending_for, "pending_for",
                                LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(pending_map, pending_fixture, pending_fields);

static void separate_messaging_store(cai_cli_pouch *sessions, const char *root,
                                     cai_error *error) {
  cai_cli_pouch messages;
  cai_cli_options options;
  pending_fixture doc;
  lc_acquire_req request;
  lc_get_res result;
  lc_lease *lease = NULL;
  lc_error local;
  char state[PATH_MAX], logs[PATH_MAX], exports[PATH_MAX];
  char id[CAI_AGENT_SESSION_ID_MAX];
  assert(snprintf(state, sizeof(state), "%s/messages", root) <
         (int)sizeof(state));
  assert(snprintf(logs, sizeof(logs), "%s/logs", root) < (int)sizeof(logs));
  assert(snprintf(exports, sizeof(exports), "%s/exports", root) <
         (int)sizeof(exports));
  assert(cai_cli_pouch_open_at(&messages, state, NULL, NULL, NULL, error) ==
         CAI_OK);
  lc_error_init(&local);
  lc_acquire_req_init(&request);
  request.ns = messages.messages_namespace;
  request.key = "inbox.fixture.split";
  request.owner = "fixture";
  request.ttl_seconds = 30;
  assert(lc_acquire(messages.sessions, &request, &lease, &local) == LC_OK);
  memset(&doc, 0, sizeof(doc));
  strcpy(doc.kind, "pending");
  strcpy(doc.pending_for, "split");
  assert(lc_lease_save(lease, &pending_map, &doc, NULL, &local) == LC_OK);
  assert(lc_lease_release(lease, NULL, &local) == LC_OK);
  save(sessions, "/workspace", "split", error);
  cai_cli_options_init(&options);
  options.log_dir = logs;
  options.export_dir = exports;
  assert(cai_cli_session_delete(sessions, &messages, &options, "/workspace",
                                "split", id, error) == CAI_OK);
  memset(&result, 0, sizeof(result));
  memset(&doc, 0, sizeof(doc));
  assert(lc_load_in_namespace(messages.sessions, messages.messages_namespace,
                              request.key, &pending_map, &doc, NULL, &result,
                              &local) == LC_OK);
  assert(result.no_content);
  lc_get_res_cleanup(&result);
  lc_error_cleanup(&local);
  cai_cli_pouch_close(&messages);
  assert(collections(sessions) == 0);
}
int main(int argc, char **argv) {
  char root[PATH_MAX], id[CAI_AGENT_SESSION_ID_MAX], *name;
  cai_cli_pouch pouch;
  cai_agent_session_event event;
  cai_cli_session metadata;
  cai_error error;
  cai_source *source = NULL;
  unsigned long long sequence;
  cleanup_state state;
  int parent, eligible;
  assert(argc == 2);
  assert(snprintf(root, sizeof(root), "%s/cai-delete.XXXXXX", argv[1]) <
         (int)sizeof(root));
  assert(mkdtemp(root) != NULL);
  cai_error_init(&error);
  assert(cai_cli_pouch_open_at(&pouch, root, NULL, NULL, NULL, &error) ==
         CAI_OK);
  save(&pouch, "/workspace", "survivor", &error);
  cai_cli_pouch_begin_use(&pouch);
  save(&pouch, "/workspace", "parent", &error);
  assert(cai_cli_pouch_prompt_open(&pouch, "parent", &error) == CAI_OK);
  assert(cai_cli_pouch_prompt_append(&pouch, "private prompt", &error) ==
         CAI_OK);
  cai_cli_pouch_prompt_close(&pouch);
  pouch.capture_session = 0;
  save(&pouch, "smith-subagent:parent", "worker-child", &error);
  save(&pouch, "smith-review:/workspace", "review-child", &error);
  save(&pouch, "smith-subagent:worker-child", "leaf-child", &error);
  memset(&event, 0, sizeof(event));
  event.sequence = 2U;
  event.type = "review_pending";
  event.data = "review-child";
  assert(pouch.store.append_event(&pouch, "/workspace", "parent", &event,
                                  &error) == CAI_OK);
  assert(collections(&pouch) == 6);
  assert(cai_cli_pouch_deletable(&pouch, "parent", &eligible, &error) ==
             CAI_OK &&
         !eligible);
  memset(&state, 0, sizeof(state));
  assert(cai_cli_pouch_delete(&pouch, "/workspace", "parent", cleanup, &state,
                              &error) == CAI_ERR_INVALID);
  assert(state.calls == 0 && collections(&pouch) == 6);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  cai_cli_pouch_end_use(&pouch);
  metadata_identity(&pouch, "parent", "/workspace", "survivor");
  assert(cai_cli_pouch_delete(&pouch, "/workspace", "parent", cleanup, &state,
                              &error) == CAI_ERR_INVALID);
  assert(state.calls == 0 && collections(&pouch) == 6);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  metadata_identity(&pouch, "parent", "/workspace", "parent");
  save(&pouch, "/workspace", "parent", &error);
  metadata_identity(&pouch, "review-child", "smith-review:/workspace",
                    "survivor");
  assert(cai_cli_pouch_delete(&pouch, "/workspace", "parent", cleanup, &state,
                              &error) == CAI_ERR_INVALID);
  assert(state.calls == 0 && collections(&pouch) == 6);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  metadata_identity(&pouch, "review-child", "smith-review:/workspace",
                    "review-child");
  save(&pouch, "smith-review:/workspace", "review-child", &error);
  state.fail = 1;
  assert(cai_cli_pouch_delete(&pouch, "/workspace", "parent", cleanup, &state,
                              &error) == CAI_ERR_TRANSPORT);
  assert(state.calls == 1 && collections(&pouch) == 6);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_cli_pouch_session(&pouch, "parent", &metadata, &error) == CAI_OK);
  state.fail = state.calls = 0;
  assert(cai_cli_pouch_delete(&pouch, "/workspace", "parent", cleanup, &state,
                              &error) == CAI_OK);
  assert(state.calls == 4 && state.seen == 15 && collections(&pouch) == 1);
  assert(cai_cli_pouch_session(&pouch, "parent", &metadata, &error) != CAI_OK);
  assert(cai_cli_pouch_session(&pouch, "review-child", &metadata, &error) !=
         CAI_OK);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(pouch.store.load_latest(&pouch, "/workspace", id, sizeof(id), &source,
                                 &sequence, &error) == CAI_OK);
  assert(source != NULL && strcmp(id, "survivor") == 0);
  cai_source_close(source);
  assert(cai_cli_pouch_delete(&pouch, "/workspace", "survivor", NULL, NULL,
                              &error) == CAI_OK);
  assert(collections(&pouch) == 0);
  separate_messaging_store(&pouch, root, &error);
  cai_cli_pouch_close(&pouch);
  cai_error_cleanup(&error);
  parent = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(parent >= 0);
  name = strrchr(root, '/');
  assert(cai_cli_remove_at(parent, name + 1) == 0);
  close(parent);
  return 0;
}
