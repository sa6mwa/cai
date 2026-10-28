/* Discover ownership from indexed child scopes and bounded review launch
 * markers. No model/context replay, commit decoding or tool execution. */
typedef struct session_deletion {
  cai_cli_pouch *pouch;
  cai_cli_session *items;
  size_t count;
  const char *review_scope;
} session_deletion;

static int deletion_add(session_deletion *plan, const cai_cli_session *item,
                        cai_error *error) {
  cai_cli_session *grown;
  size_t i;
  if (!session_component(item->id))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "unsafe child conversation ID");
  for (i = 0U; i < plan->count; i++)
    if (strcmp(plan->items[i].id, item->id) == 0)
      return CAI_OK;
  if (plan->count == (size_t)-1 / sizeof(*grown))
    return cai_cli_error(error, CAI_ERR_LIMIT, "too many child conversations");
  grown = realloc(plan->items, (plan->count + 1U) * sizeof(*grown));
  if (grown == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "collect child conversations");
  plan->items = grown;
  plan->items[plan->count++] = *item;
  return CAI_OK;
}
static int deletion_review(void *context, const cai_agent_session_event *event,
                           cai_error *error) {
  session_deletion *plan = context;
  cai_cli_session child;
  int exists, rc;
  if (strcmp(event->type, "review_pending") != 0 || event->data == NULL ||
      !session_component(event->data) ||
      strlen(event->data) >= sizeof(child.id))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid review conversation reference");
  rc = session_read(plan->pouch, event->data, &child, &exists, error);
  if (rc == CAI_OK && !exists) {
    memset(&child, 0, sizeof(child));
    strcpy(child.id, event->data);
    strcpy(child.workspace, plan->review_scope);
  }
  if (rc == CAI_OK && strcmp(child.id, event->data) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "review metadata identity does not match its reference");
  if (rc == CAI_OK && strcmp(child.workspace, plan->review_scope) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "review reference belongs to another scope");
  return rc == CAI_OK ? deletion_add(plan, &child, error) : rc;
}
static int deletion_reviews(session_deletion *plan,
                            const cai_cli_session *parent, cai_error *error) {
  collection_events read;
  lc_collection *collection = NULL;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink sink;
  lc_error local;
  lonejson_error json_error;
  char scope[PATH_MAX];
  int rc;
  if (snprintf(scope, sizeof(scope), "smith-review:%s", parent->workspace) >=
      (int)sizeof(scope))
    return cai_cli_error(error, CAI_ERR_INVALID, "review scope is too long");
  memset(&read, 0, sizeof(read));
  memset(&sink, 0, sizeof(sink));
  lc_error_init(&local);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  rc = open_history_collection(plan->pouch, parent->id, "events", 0, 1,
                               &collection, error);
  if (rc == CAI_OK && collection != NULL) {
    plan->review_scope = scope;
    read.pouch = plan->pouch;
    read.id = parent->id;
    read.map = &event_map;
    read.visit = deletion_review;
    read.visitor = plan;
    read.error = error;
    read.json = lonejson_new(NULL, &json_error);
    if (read.json == NULL)
      rc = cai_cli_lj_error(error, &json_error);
    if (rc == CAI_OK) {
      options.selector_lql = "eq{field=/type,value=review_pending}";
      options.on_metadata = collection_event_boundary;
      options.metadata_context = &read;
      sink.write = collection_event_write;
      sink.impl = &read;
      if (collection->read(collection, &options, &sink, &result, &local) !=
          LC_OK)
        rc = read.rc != CAI_OK ? read.rc : cai_cli_lc_error(error, &local);
    }
  }
  collection_event_cleanup(&read);
  lonejson_free(read.json);
  lc_collection_close(collection);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&local);
  return rc;
}
static int deletion_children(session_deletion *plan, size_t parent_index,
                             cai_error *error) {
  cai_cli_session parent = plan->items[parent_index], *children = NULL;
  char scope[256];
  size_t count = 0U, i;
  int review, rc = CAI_OK;
  for (review = 0; rc == CAI_OK && review < 2; review++) {
    snprintf(scope, sizeof(scope), "%ssmith-subagent:%s",
             review ? "smith-review:" : "", parent.id);
    rc = session_list(plan->pouch, scope, 0, &children, &count, error);
    for (i = 0U; rc == CAI_OK && i < count; i++)
      rc = deletion_add(plan, &children[i], error);
    free(children);
    children = NULL;
  }
  return rc == CAI_OK ? deletion_reviews(plan, &parent, error) : rc;
}
static int deletion_drop(cai_cli_pouch *pouch, const char *id, const char *kind,
                         cai_error *error) {
  lc_collection *collection = NULL;
  lc_error local;
  int rc;
  lc_error_init(&local);
  rc = open_history_collection(pouch, id, kind, 0, 1, &collection, error);
  if (rc == CAI_OK && collection != NULL &&
      collection->drop(collection, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_collection_close(collection);
  lc_error_cleanup(&local);
  return rc;
}
static int deletion_one(cai_cli_pouch *pouch, const cai_cli_session *item,
                        cai_error *error) {
  lc_lease *directory = NULL, *lease = NULL;
  latest_index index;
  lc_error local;
  int deleted = 0;
  int rc = latest_open(pouch, item->workspace, &directory, &index, error);
  lc_error_init(&local);
  if (rc == CAI_OK) {
    index.dirty = 1;
    rc = latest_write(pouch, directory, &index, error);
  }
  if (rc == CAI_OK)
    rc = acquire(pouch, item->id, &lease, error);
  if (rc == CAI_OK)
    rc = deletion_drop(pouch, item->id, "events", error);
  if (rc == CAI_OK)
    rc = deletion_drop(pouch, item->id, "prompts", error);
  if (rc == CAI_OK &&
      lease->delete_all_attachments(lease, &deleted, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK && lease->remove(lease, NULL, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  rc = release(lease, rc, error);
  if (rc == CAI_OK)
    rc = latest_rebuild(pouch, &index, error);
  if (rc == CAI_OK)
    rc = latest_write(pouch, directory, &index, error);
  lc_error_cleanup(&local);
  return release(directory, rc, error);
}
int cai_cli_pouch_delete(cai_cli_pouch *pouch, const char *workspace,
                         const char *id,
                         int (*cleanup)(void *, const char *, cai_error *),
                         void *context, cai_error *error) {
  session_deletion plan;
  cai_cli_session root;
  cai_cli_session_guard *guard = NULL;
  size_t i;
  int rc = history_supported(pouch, error);
  memset(&plan, 0, sizeof(plan));
  plan.pouch = pouch;
  if (rc == CAI_OK)
    rc = session_guard_open(pouch, id, 1, &guard, NULL, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_session(pouch, id, &root, error);
  if (rc == CAI_OK && strcmp(root.id, id) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "conversation metadata identity does not match its key");
  if (rc == CAI_OK && strcmp(root.workspace, workspace) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "conversation belongs to another directory");
  if (rc == CAI_OK)
    rc = deletion_add(&plan, &root, error);
  for (i = 0U; rc == CAI_OK && i < plan.count; i++)
    rc = deletion_children(&plan, i, error);
  /* Local cleanup fails before any collection is dropped. Metadata stays
   * available until each conversation's collections have been removed, so
   * interrupted cleanup can be retried by ID. */
  for (i = 0U; rc == CAI_OK && i < plan.count; i++)
    if (cleanup != NULL)
      rc = cleanup(context, plan.items[i].id, error);
  for (i = plan.count; rc == CAI_OK && i > 0U; i--)
    rc = deletion_one(pouch, &plan.items[i - 1U], error);
  if (rc == CAI_OK && unlinkat(guard->directory, id, 0) != 0 && errno != ENOENT)
    rc =
        cai_cli_error(error, CAI_ERR_TRANSPORT, "remove conversation use lock");
  free(plan.items);
  session_guard_close(guard);
  return rc;
}
