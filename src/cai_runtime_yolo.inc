/* Change only built-in confinement contexts. Never recreate the session,
 * custom tools, MCP registrations, goal state or the managed terminal slot. */
static int cai_runtime_yolo_ready_locked(cai_agent_runtime *runtime) {
  return !runtime->stopping && !runtime->turn_timing_active &&
         runtime->state != CAI_AGENT_SAMPLING &&
         runtime->state != CAI_AGENT_DISPATCHING_TOOL &&
         !runtime->settings_applying && !runtime->model_switching &&
         runtime->pending_settings.present == 0U && !runtime->compacting &&
         !runtime->subagent_active && runtime->active_review == NULL &&
         !runtime->review_launching && !runtime->review_pause_pending &&
         runtime->turn_head == NULL && runtime->steering_head == NULL &&
         runtime->goal_control_head == NULL &&
         runtime->goal_control_inflight == 0U &&
         runtime->user_shell_command == NULL &&
         runtime->shell_context_head == NULL &&
         !runtime->shell_context_applying && runtime->child_input_users == 0U &&
         !cai_terminal_tools_busy_internal(
             CAI_AGENT_IMPL(runtime->agent)->tools);
}

int cai_agent_runtime_get_yolo(cai_agent_runtime *runtime, int *enabled,
                               int *can_change, cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  pthread_mutex_lock(&runtime->lock);
  if (enabled != NULL)
    *enabled = runtime->smith_yolo != 0;
  if (can_change != NULL)
    *can_change = cai_runtime_yolo_ready_locked(runtime);
  pthread_mutex_unlock(&runtime->lock);
  return CAI_OK;
}

int cai_agent_runtime_try_set_yolo(cai_agent_runtime *runtime, int enabled,
                                   int *applied, cai_error *error) {
  cai_tool_registry *replacement = NULL, *tools;
  cai_read_tool_config read_config;
  cai_patch_tool_config patch;
  cai_view_image_tool_config image;
  unsigned long capabilities;
  int exchanged = 0, rc;
  if (applied != NULL)
    *applied = 0;
  rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (applied == NULL || (enabled != 0 && enabled != 1))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "YOLO requires a boolean mode and applied output");
  pthread_mutex_lock(&runtime->lock);
  if (runtime->stopping) {
    rc = cai_set_error(error, CAI_ERR_CANCELLED, "agent runtime is closing");
    goto done;
  }
  if (!!runtime->smith_yolo == enabled) {
    *applied = 1;
    goto done;
  }
  if (!cai_runtime_yolo_ready_locked(runtime))
    goto done;
  tools = CAI_AGENT_IMPL(runtime->agent)->tools;
  capabilities = runtime->review_mode ? runtime->preset_review_tool_capabilities
                                      : runtime->preset_tool_capabilities;
  memset(&read_config, 0, sizeof(read_config));
  read_config.root_path = runtime->workspace_directory;
  read_config.default_workdir = runtime->workspace_directory;
  read_config.yolo = enabled;
  rc = cai_tool_registry_new(&replacement, error);
  if (rc == CAI_OK && (capabilities & CAI_AGENT_PRESET_TOOL_READ_FILE) != 0UL)
    rc = cai_tool_registry_register_read_tool(replacement, &read_config, error);
  if (rc == CAI_OK && (capabilities & CAI_AGENT_PRESET_TOOL_LIST_FILES) != 0UL)
    rc = cai_tool_registry_register_list_files_tool(replacement, &read_config,
                                                    error);
  if (rc == CAI_OK &&
      (capabilities & CAI_AGENT_PRESET_TOOL_APPLY_PATCH) != 0UL) {
    memset(&patch, 0, sizeof(patch));
    patch.root_path = runtime->workspace_directory;
    patch.yolo = enabled;
    rc = cai_tool_registry_register_patch_tool(replacement, &patch, error);
  }
  if (rc == CAI_OK && cai_view_image_builtin_internal(tools)) {
    /* Never enable an absent viewer or replace a host-owned namesake. */
    memset(&image, 0, sizeof(image));
    image.root_path = runtime->workspace_directory;
    image.default_workdir = runtime->workspace_directory;
    image.additional_root_path = runtime->smith_image_directory;
    image.yolo = enabled;
    rc = cai_tool_registry_register_view_image_tool(replacement, &image, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_exchange_internal(tools, replacement, error);
    exchanged = rc == CAI_OK;
  }
  if (rc == CAI_OK)
    rc = cai_terminal_tools_yolo_internal(tools, runtime->workspace_directory,
                                          enabled, error);
  if (rc != CAI_OK && exchanged)
    (void)cai_tool_registry_exchange_internal(tools, replacement, NULL);
  if (rc == CAI_OK) {
    runtime->smith_yolo = enabled;
    *applied = 1;
    if (!runtime->logger_disabled && runtime->logger != NULL &&
        runtime->logger->info != NULL)
      runtime->logger->info(runtime->logger,
                            enabled ? "YOLO enabled" : "YOLO disabled", NULL,
                            0U);
  }
done:
  pthread_mutex_unlock(&runtime->lock);
  cai_tool_registry_destroy(replacement);
  return rc;
}
