/* Model-controlled reasoning is confined to the runtime's normal durable
 * settings boundary. No provider request, retry or synthetic prompt occurs
 * inside the tool. Configuration changes are owner-thread operations. */
typedef struct cai_reasoning_args {
  char mode[16];
} cai_reasoning_args;
static const lonejson_field cai_reasoning_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_reasoning_args, mode, "mode",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_reasoning_map, cai_reasoning_args,
                    cai_reasoning_fields);

void cai_reasoning_tool_config_init(cai_reasoning_tool_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    config->normal_effort = CAI_REASONING_EFFORT_MEDIUM;
    config->high_effort = CAI_REASONING_EFFORT_HIGH;
  }
}

static int cai_reasoning_rank(const char *effort) {
  static const char *const efforts[] = {"none", "minimal", "low", "medium",
                                        "high", "xhigh",   "max", "ultra"};
  size_t i;
  for (i = 0U; effort != NULL && i < sizeof(efforts) / sizeof(efforts[0]); i++)
    if (strcmp(effort, efforts[i]) == 0)
      return (int)i;
  return -1;
}

/* Offline lookup only. A missing list is unknown; [] advertises no choices.
 * Neither authorizes an automatic effort change. Do not guess provider support.
 */
static int cai_runtime_reasoning_supported(cai_agent_runtime *runtime,
                                           const char *model,
                                           const char *effort) {
  cai_model_catalog *catalog = NULL;
  const cai_model_catalog_entry *entry;
  cai_error error;
  size_t i;
  int supported = 0;
  cai_error_init(&error);
  if (cai_client_list_models(runtime->client, CAI_MODEL_CATALOG_REFRESH_OFFLINE,
                             &catalog, &error) == CAI_OK) {
    entry = cai_model_catalog_find(catalog, model);
    if (entry != NULL && entry->reasoning_efforts_known)
      for (i = 0U; i < entry->reasoning_effort_count; i++)
        if (strcmp(entry->reasoning_efforts[i], effort) == 0)
          supported = 1;
  }
  cai_model_catalog_close(catalog);
  cai_error_cleanup(&error);
  return supported;
}

static int
cai_runtime_reasoning_receipt_locked(cai_agent_runtime *runtime, int mode,
                                     const char *requested, const char *before,
                                     const char *after, const char *reason,
                                     const char *call, cai_error *error) {
  cai_runtime_execution_stream receipt;
  char text[512];
  int length, rc = CAI_OK;
  const char *from = before != NULL ? before : "provider default";
  const char *to = after != NULL ? after : "provider default";
  if (strcmp(from, to) == 0)
    length =
        snprintf(text, sizeof(text), "requested %s (%s): unchanged at %s%s%s%s",
                 mode == 2 ? "high" : "normal", requested, to,
                 reason != NULL ? " (" : "", reason != NULL ? reason : "",
                 reason != NULL ? ")" : "");
  else
    length = snprintf(text, sizeof(text), "requested %s (%s): %s -> %s%s%s%s",
                      mode == 2 ? "high" : "normal", requested, from, to,
                      reason != NULL ? " (" : "", reason != NULL ? reason : "",
                      reason != NULL ? ")" : "");
  if (length < 0 || (size_t)length >= sizeof(text))
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "reasoning receipt exceeds limit");
  if (runtime->record_transcript) {
    cai_runtime_execution_init(&receipt, runtime, "reasoning.receipt",
                               "set_reasoning", call, NULL);
    memcpy(receipt.bytes, text, (size_t)length);
    receipt.length = (size_t)length;
    rc = cai_runtime_execution_emit_locked(&receipt, receipt.length, 1, error);
  }
  if (rc == CAI_OK)
    rc = cai_runtime_enqueue_locked(
        runtime, CAI_AGENT_EVENT_REASONING_TOOL_RESULT, text, (size_t)length,
        "set_reasoning", call, runtime->state, error);
  return rc;
}

/* Caller holds runtime->lock, serializing warning identity and configuration.
 */
static void cai_runtime_reasoning_warn(cai_agent_runtime *runtime,
                                       const char *model, const char *effort,
                                       int high) {
  unsigned int bit = high ? 2U : 1U;
  if (runtime->reasoning_warning_model == NULL ||
      strcmp(runtime->reasoning_warning_model, model) != 0) {
    cai_free_mem(NULL, runtime->reasoning_warning_model);
    runtime->reasoning_warning_model = cai_strdup(NULL, model);
    runtime->reasoning_warning_mask = 0U;
  }
  if ((runtime->reasoning_warning_mask & bit) == 0U &&
      !runtime->logger_disabled && runtime->logger != NULL &&
      runtime->logger->warn != NULL) {
    pslog_field fields[4];
    char message[1024];
    size_t i;
    memset(fields, 0, sizeof(fields));
    fields[0].key = "event";
    fields[0].as.string_value = "cai.reasoning_tool.unavailable";
    fields[1].key = "model";
    fields[1].as.string_value = model;
    fields[2].key = "mode";
    fields[2].as.string_value = high ? "high" : "normal";
    fields[3].key = "effort";
    fields[3].as.string_value = effort;
    for (i = 0U; i < 4U; i++) {
      fields[i].type = PSLOG_FIELD_STRING;
      fields[i].key_len = strlen(fields[i].key);
      fields[i].value_len = strlen(fields[i].as.string_value);
    }
    snprintf(message, sizeof(message),
             "Reasoning tool: model %s does not advertise %s effort %s. "
             "That mode will do nothing. Set --%s-reasoning-effort, "
             "CAI_%s_REASONING_EFFORT, [agent] %s_reasoning_effort, or "
             "/reasoning-tool %s LEVEL to a supported effort; supply model "
             "metadata if its choices are unavailable.",
             model, high ? "high" : "normal", effort, high ? "high" : "normal",
             high ? "HIGH" : "NORMAL", high ? "high" : "normal",
             high ? "high" : "normal");
    runtime->logger->warn(runtime->logger, message, fields, 4U);
  }
  runtime->reasoning_warning_mask |= bit;
}

static void cai_runtime_reasoning_check(cai_agent_runtime *runtime) {
  const char *model;
  pthread_mutex_lock(&runtime->lock);
  if (runtime->reasoning_tool.enabled && runtime->agent != NULL) {
    model = CAI_AGENT_IMPL(runtime->agent)->model;
    if (!cai_runtime_reasoning_supported(runtime, model,
                                         runtime->reasoning_tool.normal_effort))
      cai_runtime_reasoning_warn(runtime, model,
                                 runtime->reasoning_tool.normal_effort, 0);
    if (!cai_runtime_reasoning_supported(runtime, model,
                                         runtime->reasoning_tool.high_effort))
      cai_runtime_reasoning_warn(runtime, model,
                                 runtime->reasoning_tool.high_effort, 1);
  }
  pthread_mutex_unlock(&runtime->lock);
}

static int cai_runtime_reasoning_tool(void *context, const char *arguments,
                                      cai_sink *output, cai_error *error) {
  cai_agent_runtime *runtime = context;
  cai_reasoning_args args;
  lonejson_error json_error;
  cai_runtime_settings_owned candidate;
  cai_agent_runtime_settings selected;
  const char *model, *effort;
  const char *unchanged = NULL;
  int high, rc = CAI_OK;
  memset(&args, 0, sizeof(args));
  memset(&candidate, 0, sizeof(candidate));
  if (CAI_LJ->parse_cstr(CAI_LJ, &cai_reasoning_map, &args, arguments,
                         &json_error) != LONEJSON_STATUS_OK ||
      (strcmp(args.mode, "high") != 0 && strcmp(args.mode, "normal") != 0))
    return cai_set_error(error, CAI_ERR_INVALID, "mode must be normal or high");
  high = strcmp(args.mode, "high") == 0;
  pthread_mutex_lock(&runtime->lock);
  model = runtime->pending_settings.model != NULL
              ? runtime->pending_settings.model
              : CAI_AGENT_IMPL(runtime->agent)->model;
  effort = high ? runtime->reasoning_tool.high_effort
                : runtime->reasoning_tool.normal_effort;
  if (!runtime->reasoning_tool.enabled || runtime->stopping ||
      (runtime->pending_settings.present &
       CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT) != 0U) {
    /* An explicit host/user update wins over a model request. */
    unchanged = "another settings change is pending or the tool is disabled";
  } else if (!cai_runtime_reasoning_supported(runtime, model, effort)) {
    cai_runtime_reasoning_warn(runtime, model, effort, high);
    unchanged = "not advertised by model";
  } else if (high && cai_reasoning_rank(
                         CAI_AGENT_IMPL(runtime->agent)->reasoning_effort) >=
                         cai_reasoning_rank(effort)) {
    /* An escalation never lowers an effort already above the mapped high. */
    unchanged = "already at or above requested effort";
  } else {
    selected = cai_runtime_settings_borrow(&runtime->pending_settings);
    selected.present |= CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT;
    selected.reasoning_effort = effort;
    rc = cai_runtime_settings_copy(&candidate, &selected, NULL, error);
    if (rc == CAI_OK) {
      cai_runtime_settings_clear(&runtime->pending_settings);
      runtime->pending_settings = candidate;
      runtime->pending_settings_failed = 0;
      runtime->reasoning_tool_pending = high ? 2 : 1;
      runtime->reasoning_receipt_mode = high ? 2 : 1;
      snprintf(runtime->reasoning_receipt_before,
               sizeof(runtime->reasoning_receipt_before), "%s",
               CAI_AGENT_IMPL(runtime->agent)->reasoning_effort != NULL
                   ? CAI_AGENT_IMPL(runtime->agent)->reasoning_effort
                   : "provider default");
      snprintf(runtime->reasoning_receipt_requested,
               sizeof(runtime->reasoning_receipt_requested), "%s", effort);
      cai_free_mem(NULL, runtime->reasoning_receipt_call);
      runtime->reasoning_receipt_call = runtime->reasoning_current_call;
      runtime->reasoning_current_call = NULL;
      runtime->pending_after_active = 1;
      pthread_cond_broadcast(&runtime->condition);
    }
  }
  if (rc == CAI_OK && unchanged != NULL)
    rc = cai_runtime_reasoning_receipt_locked(
        runtime, high ? 2 : 1, effort,
        CAI_AGENT_IMPL(runtime->agent)->reasoning_effort,
        CAI_AGENT_IMPL(runtime->agent)->reasoning_effort, unchanged,
        runtime->reasoning_current_call, error);
  pthread_mutex_unlock(&runtime->lock);
  if (rc != CAI_OK)
    return rc;
  /* Acknowledge the request without exposing effective settings or whether
   * they changed. Unsupported-effort warnings are for the host/user only. */
  return cai_sink_write(output, "ok", 2U, error);
}

int cai_agent_runtime_get_reasoning_tool(cai_agent_runtime *runtime,
                                         cai_reasoning_tool_config *out,
                                         cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "reasoning tool output required");
  pthread_mutex_lock(&runtime->lock);
  out->enabled = runtime->reasoning_tool.enabled;
  out->normal_effort = runtime->reasoning_tool.normal_effort;
  out->high_effort = runtime->reasoning_tool.high_effort;
  pthread_mutex_unlock(&runtime->lock);
  return CAI_OK;
}

int cai_agent_runtime_set_reasoning_tool(
    cai_agent_runtime *runtime, const cai_reasoning_tool_config *config,
    cai_error *error) {
  const char *normal, *high;
  char *normal_copy, *high_copy;
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (config == NULL || (runtime->review_mode && config->enabled))
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "reasoning tool configuration is invalid for this runtime");
  normal = config->normal_effort != NULL ? config->normal_effort : "medium";
  high = config->high_effort != NULL ? config->high_effort : "high";
  if (cai_reasoning_rank(normal) < 0 || cai_reasoning_rank(high) < 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid reasoning effort mapping");
  normal_copy = cai_strdup(NULL, normal);
  high_copy = cai_strdup(NULL, high);
  if (normal_copy == NULL || high_copy == NULL) {
    cai_free_mem(NULL, normal_copy);
    cai_free_mem(NULL, high_copy);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "copy reasoning tool configuration");
  }
  pthread_mutex_lock(&runtime->lock);
  if (runtime->stopping || runtime->turn_head != NULL ||
      runtime->turn_timing_active || runtime->settings_applying ||
      runtime->model_switching || runtime->state == CAI_AGENT_SAMPLING ||
      runtime->state == CAI_AGENT_DISPATCHING_TOOL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "Configure the reasoning tool between turns.");
  } else if (!!config->enabled != !!runtime->reasoning_tool.enabled) {
    if (config->enabled)
      rc = cai_agent_register_raw_tool(
          runtime->agent, "set_reasoning",
          "Request mode=high when the task is complex and needs more "
          "reasoning. "
          "Only raise effort unless explicitly instructed to return to normal. "
          "Never automatically lower effort after a difficult step.",
          "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\","
          "\"enum\":[\"normal\",\"high\"]}},\"required\":[\"mode\"],"
          "\"additionalProperties\":false}",
          1, cai_runtime_reasoning_tool, runtime, error);
    else
      rc = cai_tool_registry_remove_internal(
          CAI_AGENT_IMPL(runtime->agent)->tools, "set_reasoning", error);
  }
  if (rc == CAI_OK) {
    cai_free_mem(NULL, runtime->reasoning_tool.normal_effort);
    cai_free_mem(NULL, runtime->reasoning_tool.high_effort);
    runtime->reasoning_tool.normal_effort = normal_copy;
    runtime->reasoning_tool.high_effort = high_copy;
    runtime->reasoning_tool.enabled = !!config->enabled;
    runtime->reasoning_warning_mask = 0U;
  }
  pthread_mutex_unlock(&runtime->lock);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, normal_copy);
    cai_free_mem(NULL, high_copy);
  } else if (runtime->worker_started ||
             !cai_client_has_live_model_catalog(runtime->client)) {
    /* Live catalogs are populated at the first turn's metadata boundary.
     * A cold cache at open is not evidence of unsupported defaults. */
    cai_runtime_reasoning_check(runtime);
  }
  return rc;
}
