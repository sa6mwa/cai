typedef struct export_span {
  struct export_span *next;
  uint64_t after, through;
} export_span;
typedef struct export_selection {
  collection_events *read;
  cai_agent_runtime *runtime;
  export_span *first, *last;
  int adjacent;
} export_selection;
static int export_select(void *context, const cai_agent_session_event *event,
                         cai_error *error) {
  export_selection *selection = context;
  export_span *span;
  if (!cai_agent_runtime_history_event_visible(selection->runtime,
                                               event->sequence)) {
    selection->adjacent = 0;
    return CAI_OK;
  }
  /* Commits share the collection with observations. Never bridge their gaps:
   * portable exports include one retained-context initialization, not prior
   * context edits which could disclose trimmed content. */
  if (!selection->adjacent ||
      selection->last->through + 1U != selection->read->position) {
    span = calloc(1U, sizeof(*span));
    if (span == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "allocate export range");
    span->after = selection->read->position - 1U;
    if (selection->last != NULL)
      selection->last->next = span;
    else
      selection->first = span;
    selection->last = span;
  }
  selection->last->through = selection->read->position;
  selection->adjacent = 1;
  return CAI_OK;
}
int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id,
                         cai_agent_runtime *runtime, cai_sink *sink,
                         cai_error *error) {
  lc_collection *collection = NULL;
  collection_events read;
  export_selection selection;
  export_span *span;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink output;
  lc_error detail;
  lonejson_error local;
  lonejson_config config = lonejson_default_config();
  int rc;
  memset(&read, 0, sizeof(read));
  memset(&selection, 0, sizeof(selection));
  memset(&output, 0, sizeof(output));
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  lc_error_init(&detail);
  config.clear_destination_by_default = 0;
  config.json_value_max_total_bytes = 0U;
  config.json_value_max_string_bytes = 0U;
  read.map = &export_identity_map;
  read.discard_data = 1;
  read.pouch = pouch;
  read.id = id;
  read.error = error;
  read.visit = export_select;
  read.visitor = &selection;
  selection.read = &read;
  selection.runtime = runtime;
  rc = history_collection(pouch, id, "events", 0, &collection, error);
  if (rc == CAI_OK) {
    read.json = lonejson_new(&config, &local);
    if (read.json == NULL)
      rc = cai_cli_lj_error(error, &local);
  }
  if (rc == CAI_OK) {
    options.selector_lql = "range{field=/sequence,gt=0}";
    options.on_metadata = collection_event_boundary;
    options.metadata_context = &read;
    output.write = collection_event_write;
    output.impl = &read;
    if (collection->read(collection, &options, &output, &result, &detail) !=
        LC_OK)
      rc = read.rc != CAI_OK ? read.rc : cai_cli_lc_error(error, &detail);
  }
  collection_event_cleanup(&read);
  lonejson_free(read.json);
  while (selection.first != NULL) {
    span = selection.first;
    if (rc == CAI_OK)
      rc = collection_copy(collection, span->after, span->through, sink, error);
    selection.first = span->next;
    free(span);
  }
  if (collection != NULL)
    collection->close(collection);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&detail);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_export_journal_state(runtime, sink, error);
  return rc;
}
