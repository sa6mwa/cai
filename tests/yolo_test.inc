#include "cai_process_environment.h"

static int yolo_host_tool(void *context, const char *arguments, cai_sink *sink,
                          cai_error *error) {
  (void)context;
  return cai_sink_write(sink, arguments, strlen(arguments), error);
}

static int yolo_capture_agent(void *context, cai_agent *agent,
                              cai_error *error) {
  *(cai_agent **)context = agent;
  return cai_agent_register_raw_tool(agent, "host_tool", "Host tool",
                                     "{\"type\":\"object\"}", 0, yolo_host_tool,
                                     NULL, error);
}

static void *yolo_wrong_thread(void *context) {
  cai_error error;
  int applied = 1;
  cai_error_init(&error);
  if (cai_agent_runtime_get_yolo(context, NULL, NULL, &error) !=
          CAI_ERR_INVALID ||
      cai_agent_runtime_try_set_yolo(context, 1, &applied, &error) !=
          CAI_ERR_INVALID ||
      applied)
    context = NULL;
  cai_error_cleanup(&error);
  return context;
}

static void test_runtime_yolo(test_state *state) {
  static const unsigned char png[] = {
      0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
      0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U,
      0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1fU,
      0x15U, 0xc4U, 0x89U, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x44U, 0x41U,
      0x54U, 0x08U, 0xd7U, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xf0U, 0x1fU, 0x00U,
      0x05U, 0x00U, 0x01U, 0xffU, 0x89U, 0x99U, 0x07U, 0x00U, 0x00U, 0x00U,
      0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U};
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/cai-runtime-yolo-XXXXXX";
  char work[PATH_MAX], outside[PATH_MAX], images[PATH_MAX], added[PATH_MAX];
  char image[PATH_MAX];
  char id[128];
  cai_client_config cc;
  cai_agent_runtime_config config;
  cai_agent_preset preset;
  cai_terminal_tool_config terminal;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_agent *agent = NULL;
  cai_tool_registry *tools;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  write_state output;
  test_env_snapshot secret;
  pthread_t thread;
  void *thread_result = NULL;
  int enabled, ready, applied, mode, rc, policy_calls = 0;
  size_t count;
  cai_error_init(&error);
  if (mkdtemp(root) == NULL) {
    test_fail(state, "runtime_yolo_fixture", "mkdtemp");
    return;
  }
  snprintf(work, sizeof(work), "%s/work", root);
  snprintf(outside, sizeof(outside), "%s/outside", root);
  snprintf(images, sizeof(images), "%s/images", root);
  snprintf(added, sizeof(added), "%s/added", root);
  snprintf(image, sizeof(image), "%s/outside.png", root);
  mkdir(work, 0700);
  mkdir(images, 0700);
  write_bytes_or_die(outside, (const unsigned char *)"outside", 7U);
  write_bytes_or_die(image, png, sizeof(png));
  test_env_capture(&secret, "CAI_TEST_YOLO_SECRET");
  setenv("CAI_TEST_YOLO_SECRET", "full-env", 1);
  cai_client_config_init(&cc);
  cc.api_key = "fixture";
  cc.base_url = "http://127.0.0.1:1/v1";
  cc.logger_disabled = 1;
  expect_int(state, "runtime_yolo_client",
             cai_client_open(&cc, &client, &error), CAI_OK);
  if (client == NULL)
    goto cleanup;
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = work;
  config.model = "gpt-6-luna";
  config.disable_chrome = config.disable_web_search = config.disable_skills = 1;
  config.disable_default_session_store = 1;
  config.register_agent_tools = yolo_capture_agent;
  config.register_agent_tools_context = &agent;
  config.image_directory = images;
  memset(&terminal, 0, sizeof(terminal));
  terminal.policy = test_terminal_policy;
  terminal.policy_context = &policy_calls;
  config.terminal_tool_config = &terminal;
  expect_int(state, "runtime_yolo_open",
             cai_agent_runtime_open(client, &config, &runtime, &error), CAI_OK);
  if (runtime == NULL)
    goto cleanup;
  tools = CAI_AGENT_IMPL(agent)->tools;
  count = cai_tool_registry_count(tools);
  snprintf(id, sizeof(id), "%s", cai_agent_runtime_session_id(runtime));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = test_write;
  callbacks.context = &output;
  expect_int(state, "runtime_yolo_sink",
             cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
  expect_int(state, "runtime_yolo_invalid_boolean",
             cai_agent_runtime_try_set_yolo(runtime, 2, &applied, &error),
             CAI_ERR_INVALID);
  expect_int(state, "runtime_yolo_invalid_clears_applied", applied, 0);
  expect_int(state, "runtime_yolo_requires_output",
             cai_agent_runtime_try_set_yolo(runtime, 1, NULL, &error),
             CAI_ERR_INVALID);
  expect_int(state, "runtime_yolo_thread",
             pthread_create(&thread, NULL, yolo_wrong_thread, runtime), 0);
  pthread_join(thread, &thread_result);
  expect_int(state, "runtime_yolo_owner_only", thread_result == runtime, 1);
  /* Off -> on -> off: observable file, patch and environment policy, while
   * preserving the registry, custom tools, session and terminal slot. */
  for (mode = 0; mode < 3; mode++) {
    enabled = mode == 1;
    expect_int(
        state, "runtime_yolo_set",
        cai_agent_runtime_try_set_yolo(runtime, enabled, &applied, &error),
        CAI_OK);
    expect_int(state, "runtime_yolo_applied", applied, 1);
    expect_int(state, "runtime_yolo_get",
               cai_agent_runtime_get_yolo(runtime, &applied, &ready, &error),
               CAI_OK);
    expect_int(state, "runtime_yolo_effective", applied, enabled);
    expect_int(state, "runtime_yolo_idle", ready, 1);
    expect_str(state, "runtime_yolo_same_session",
               cai_agent_runtime_session_id(runtime), id);
    expect_int(state, "runtime_yolo_same_registry",
               tools == CAI_AGENT_IMPL(agent)->tools, 1);
    expect_int(state, "runtime_yolo_same_inventory",
               cai_tool_registry_count(tools), count);
    memset(&output, 0, sizeof(output));
    expect_int(state, "runtime_yolo_custom_preserved",
               cai_tool_registry_run(tools, "host_tool", "{}", sink, &error),
               CAI_OK);
    memset(&output, 0, sizeof(output));
    expect_int(state, "runtime_yolo_read_policy",
               cai_tool_registry_run(tools, "read_file",
                                     "{\"path\":\"../outside\"}", sink, &error),
               enabled ? CAI_OK : CAI_ERR_INVALID);
    memset(&output, 0, sizeof(output));
    expect_int(state, "runtime_yolo_list_policy",
               cai_tool_registry_run(tools, "list_files", "{\"path\":\"..\"}",
                                     sink, &error),
               enabled ? CAI_OK : CAI_ERR_INVALID);
    memset(&output, 0, sizeof(output));
    rc = cai_tool_registry_run(
        tools, "apply_patch",
        "*** Begin Patch\n*** Add File: ../added\n+ok\n*** End Patch\n", sink,
        &error);
    expect_int(state, "runtime_yolo_patch_policy", rc,
               enabled ? CAI_OK : CAI_ERR_INVALID);
    if (enabled)
      unlink(added);
    memset(&output, 0, sizeof(output));
    expect_int(state, "runtime_yolo_image_policy",
               cai_tool_registry_run(tools, "view_image",
                                     "{\"path\":\"../outside.png\"}", sink,
                                     &error),
               enabled ? CAI_OK : CAI_ERR_INVALID);
    memset(&output, 0, sizeof(output));
    expect_int(state, "runtime_yolo_environment_command",
               cai_tool_registry_run(
                   tools, "exec_command",
                   "{\"cmd\":\"printf "
                   "${CAI_TEST_YOLO_SECRET-filtered}\",\"yield_time_ms\":1000}",
                   sink, &error),
               CAI_OK);
    expect_substr(state, "runtime_yolo_environment", output.buffer,
                  enabled ? "full-env" : "filtered");
    memset(&output, 0, sizeof(output));
    expect_int(
        state, "runtime_yolo_terminal_workdir",
        cai_tool_registry_run(
            tools, "exec_command",
            "{\"cmd\":\"pwd\",\"workdir\":\"..\",\"yield_time_ms\":1000}", sink,
            &error),
        enabled ? CAI_OK : CAI_ERR_INVALID);
  }
  expect_int(state, "runtime_yolo_restores_policy_callback", policy_calls, 2);
  memset(&output, 0, sizeof(output));
  expect_int(state, "runtime_yolo_restores_host_denial",
             cai_tool_registry_run(tools, "exec_command",
                                   "{\"cmd\":\"forbidden\"}", sink, &error),
             CAI_ERR_INVALID);
  expect_int(
      state, "runtime_yolo_shell_begin",
      cai_agent_runtime_user_shell_begin(runtime, "user shell", 0, &error),
      CAI_OK);
  expect_int(state, "runtime_yolo_shell_defer",
             cai_agent_runtime_try_set_yolo(runtime, 1, &applied, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_shell_not_applied", applied, 0);
  expect_int(state, "runtime_yolo_shell_finish",
             cai_agent_runtime_user_shell_finish(runtime, 0, 0ULL, &error),
             CAI_OK);
  /* A live managed command blocks switching, without losing its stdin or ID. */
  memset(&output, 0, sizeof(output));
  expect_int(
      state, "runtime_yolo_start_terminal",
      cai_tool_registry_run(
          tools, "exec_command",
          "{\"cmd\":\"read answer; printf got-$answer\",\"yield_time_ms\":1}",
          sink, &error),
      CAI_OK);
  expect_int(state, "runtime_yolo_busy_get",
             cai_agent_runtime_get_yolo(runtime, &enabled, &ready, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_terminal_busy", ready, 0);
  expect_int(state, "runtime_yolo_defer",
             cai_agent_runtime_try_set_yolo(runtime, 1, &applied, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_not_applied", applied, 0);
  expect_int(state, "runtime_yolo_busy_idempotent",
             cai_agent_runtime_try_set_yolo(runtime, 0, &applied, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_noop_applied", applied, 1);
  memset(&output, 0, sizeof(output));
  expect_int(state, "runtime_yolo_retained_terminal",
             cai_tool_registry_run(tools, "write_stdin",
                                   "{\"session_id\":\"terminal-1\",\"chars\":"
                                   "\"retained\\n\",\"yield_time_ms\":1000}",
                                   sink, &error),
             CAI_OK);
  expect_substr(state, "runtime_yolo_retained_output", output.buffer,
                "got-retained");
  expect_int(state, "runtime_yolo_after_terminal",
             cai_agent_runtime_try_set_yolo(runtime, 1, &applied, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_after_terminal_applied", applied, 1);
  /* Failure while staging a viewer restores every preceding file policy. */
  rmdir(images);
  expect_int(state, "runtime_yolo_atomic_failure",
             cai_agent_runtime_try_set_yolo(runtime, 0, &applied, &error),
             CAI_ERR_INVALID);
  expect_int(state, "runtime_yolo_failure_not_applied", applied, 0);
  expect_int(state, "runtime_yolo_rollback_get",
             cai_agent_runtime_get_yolo(runtime, &enabled, NULL, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_rollback_mode", enabled, 1);
  memset(&output, 0, sizeof(output));
  expect_int(state, "runtime_yolo_rollback_read",
             cai_tool_registry_run(tools, "read_file",
                                   "{\"path\":\"../outside\"}", sink, &error),
             CAI_OK);
  /* An unavailable workspace must likewise leave the policy unchanged. */
  mkdir(images, 0700);
  expect_int(state, "runtime_yolo_remove_workspace", rmdir(work), 0);
  expect_int(state, "runtime_yolo_terminal_rollback",
             cai_agent_runtime_try_set_yolo(runtime, 0, &applied, &error),
             CAI_ERR_INVALID);
  mkdir(work, 0700);
  expect_int(state, "runtime_yolo_rollback_get_again",
             cai_agent_runtime_get_yolo(runtime, &enabled, NULL, &error),
             CAI_OK);
  expect_int(state, "runtime_yolo_rollback_terminal_mode", enabled, 1);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  /* A restricted preset must stay restricted in capability, regardless of
   * whether its remaining tools may escape the workspace. */
  cai_agent_preset_from_smith(&preset);
  preset.tool_capabilities = 0UL;
  config.preset_descriptor = &preset;
  config.preset = NULL;
  config.disable_terminal = 1;
  expect_int(state, "runtime_yolo_disabled_open",
             cai_agent_runtime_open(client, &config, &runtime, &error), CAI_OK);
  if (runtime != NULL) {
    tools = CAI_AGENT_IMPL(agent)->tools;
    count = cai_tool_registry_count(tools);
    expect_int(state, "runtime_yolo_disabled_change",
               cai_agent_runtime_try_set_yolo(runtime, 1, &applied, &error),
               CAI_OK);
    expect_int(state, "runtime_yolo_disabled_inventory",
               cai_tool_registry_count(tools), count);
    expect_int(state, "runtime_yolo_disabled_applied", applied, 1);
  }
cleanup:
  cai_sink_close(sink);
  cai_agent_runtime_close(runtime);
  cai_client_close(client);
  test_env_restore(&secret);
  cai_error_cleanup(&error);
  unlink(outside);
  unlink(image);
  rmdir(images);
  rmdir(work);
  rmdir(root);
}

static const char *yolo_env(char **environment, const char *name) {
  size_t i, n = strlen(name);
  for (i = 0U; environment[i] != NULL; i++)
    if (strncmp(environment[i], name, n) == 0 && environment[i][n] == '=')
      return environment[i] + n + 1U;
  return NULL;
}
static void test_yolo_environment(test_state *state) {
  static const char *const names[] = {
      "HOME", "XDG_CONFIG_HOME", "XDG_FUTURE_VALUE", "CAI_TEST_YOLO_SECRET"};
  static const char *const values[] = {"/real user/home", "relative-xdg", "",
                                       "retained-only-in-yolo"};
  static const char *const defaults[] = {"LANG=C", NULL};
  test_env_snapshot saved[4];
  cai_error error;
  char **environment = NULL;
  size_t i;
  int mode;
  cai_error_init(&error);
  for (i = 0U; i < 4U; i++) {
    test_env_capture(&saved[i], names[i]);
    setenv(names[i], values[i], 1);
  }
  for (mode = 0; mode < 2; mode++) {
    expect_int(
        state, "yolo_environment_snapshot",
        cai_process_environment_new(mode, defaults, &environment, &error),
        CAI_OK);
    if (environment != NULL) {
      for (i = 0U; i < 3U; i++)
        expect_str(state, "identity_environment_unchanged",
                   yolo_env(environment, names[i]), values[i]);
      expect_int(state, "filtered_environment_secret",
                 yolo_env(environment, names[3]) != NULL, mode);
      setenv("HOME", "/changed-after-snapshot", 1);
      expect_str(state, "environment_snapshot_owned",
                 yolo_env(environment, "HOME"), values[0]);
      setenv("HOME", values[0], 1);
      cai_process_environment_free(environment);
      environment = NULL;
    }
  }
  unsetenv("HOME");
  unsetenv("XDG_CONFIG_HOME");
  expect_int(state, "environment_unset_snapshot",
             cai_process_environment_new(0, defaults, &environment, &error),
             CAI_OK);
  if (environment != NULL) {
    expect_int(state, "unset_home_not_fabricated",
               yolo_env(environment, "HOME") == NULL, 1);
    expect_int(state, "unset_xdg_not_fabricated",
               yolo_env(environment, "XDG_CONFIG_HOME") == NULL, 1);
  }
  cai_process_environment_free(environment);
  for (i = 0U; i < 4U; i++)
    test_env_restore(&saved[i]);
  cai_error_cleanup(&error);
}

static void test_yolo_tools(test_state *state) {
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/cai-yolo-XXXXXX";
  char work[PATH_MAX], outside[PATH_MAX], linked[PATH_MAX], added[PATH_MAX];
  char target[PATH_MAX], shortcut[PATH_MAX], alias_added[PATH_MAX];
  char missing[PATH_MAX], nested[PATH_MAX];
  cai_read_tool_config read;
  cai_patch_tool_config patch;
  cai_exec_tool_config exec;
  cai_terminal_tool_config terminal;
  cai_tool_registry *registry = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  write_state output;
  int mode, policy_calls = 0;
  cai_error_init(&error);
  if (mkdtemp(root) == NULL) {
    test_fail(state, "yolo_temp", "mkdtemp");
    return;
  }
  snprintf(work, sizeof(work), "%s/work", root);
  snprintf(outside, sizeof(outside), "%s/outside", root);
  snprintf(linked, sizeof(linked), "%s/hardlink", root);
  snprintf(added, sizeof(added), "%s/added", root);
  snprintf(target, sizeof(target), "%s/target", root);
  snprintf(shortcut, sizeof(shortcut), "%s/work/shortcut", root);
  snprintf(alias_added, sizeof(alias_added), "%s/alias-added", root);
  snprintf(missing, sizeof(missing), "%s/missing", root);
  snprintf(nested, sizeof(nested), "%s/missing/created", root);
  mkdir(work, 0700);
  write_bytes_or_die(outside, (const unsigned char *)"outside-content\n", 16U);
  expect_int(state, "yolo_hardlink", link(outside, linked), 0);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = test_write;
  callbacks.context = &output;
  expect_int(state, "yolo_sink",
             cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
  for (mode = 0; mode < 2; mode++) {
    memset(&read, 0, sizeof(read));
    read.root_path = work;
    read.yolo = mode;
    memset(&output, 0, sizeof(output));
    expect_int(state, "yolo_registry", cai_tool_registry_new(&registry, &error),
               CAI_OK);
    expect_int(state, "yolo_read_register",
               cai_tool_registry_register_read_tool(registry, &read, &error),
               CAI_OK);
    expect_int(
        state, "yolo_list_register",
        cai_tool_registry_register_list_files_tool(registry, &read, &error),
        CAI_OK);
    expect_int(state, "yolo_outside_read",
               cai_tool_registry_run(registry, "read_file",
                                     "{\"path\":\"../outside\"}", sink, &error),
               mode ? CAI_OK : CAI_ERR_INVALID);
    if (mode)
      expect_substr(state, "yolo_read_content", output.buffer,
                    "outside-content");
    memset(&output, 0, sizeof(output));
    expect_int(state, "yolo_outside_list",
               cai_tool_registry_run(registry, "list_files",
                                     "{\"path\":\"..\"}", sink, &error),
               mode ? CAI_OK : CAI_ERR_INVALID);
    if (mode)
      expect_substr(state, "yolo_list_content", output.buffer, "outside");
    cai_tool_registry_destroy(registry);
    registry = NULL;
    memset(&patch, 0, sizeof(patch));
    patch.root_path = work;
    patch.yolo = mode;
    memset(&output, 0, sizeof(output));
    expect_int(state, "yolo_outside_patch",
               cai_apply_patch(&patch,
                               "*** Begin Patch\n*** Add File: "
                               "../added\n+created\n*** End Patch\n",
                               sink, &error),
               mode ? CAI_OK : CAI_ERR_INVALID);
    expect_int(state, "yolo_patch_effect", access(added, F_OK) == 0, mode);
  }
  memset(&exec, 0, sizeof(exec));
  memset(&patch, 0, sizeof(patch));
  patch.root_path = work;
  patch.yolo = 1;
  mkdir(target, 0700);
  expect_int(state, "yolo_symlink_fixture", symlink(target, shortcut), 0);
  memset(&output, 0, sizeof(output));
  expect_int(state, "yolo_symlink_parent_semantics",
             cai_apply_patch(&patch,
                             "*** Begin Patch\n*** Add File: "
                             "shortcut/../alias-added\n+alias\n*** End Patch\n",
                             sink, &error),
             CAI_OK);
  expect_int(state, "yolo_symlink_destination", access(alias_added, F_OK), 0);
  memset(&output, 0, sizeof(output));
  expect_int(
      state, "yolo_missing_parent_semantics",
      cai_apply_patch(&patch,
                      "*** Begin Patch\n*** Add File: "
                      "../missing/deep/../created\n+nested\n*** End Patch\n",
                      sink, &error),
      CAI_OK);
  expect_int(state, "yolo_missing_destination", access(nested, F_OK), 0);
  memset(&output, 0, sizeof(output));
  expect_int(
      state, "yolo_patch_existing_hardlink",
      cai_apply_patch(
          &patch,
          "*** Begin Patch\n*** Update File: "
          "../outside\n@@\n-outside-content\n+outside-content-updated\n*** End "
          "Patch\n",
          sink, &error),
      CAI_OK);
  exec.root_path = work;
  exec.yolo = 1;
  exec.bwrap_path = "/no-such-bubblewrap";
  expect_int(
      state, "yolo_exec_bypass",
      run_exec_tool_case(state, "yolo_exec", &exec,
                         "{\"cmd\":\"printf yolo-exec; cat ../outside\"}",
                         CAI_OK, &output, &error),
      CAI_OK);
  expect_substr(state, "yolo_exec_host_access", output.buffer,
                "outside-content");
  expect_substr(state, "yolo_exec_mode", output.buffer,
                "\"sandbox\":\"disabled\"");
  memset(&terminal, 0, sizeof(terminal));
  terminal.root_path = work;
  terminal.yolo = 1;
  terminal.policy = test_terminal_policy;
  terminal.policy_context = &policy_calls;
  expect_int(state, "yolo_terminal_registry",
             cai_tool_registry_new(&registry, &error), CAI_OK);
  expect_int(
      state, "yolo_terminal_register",
      cai_tool_registry_register_terminal_tools(registry, &terminal, &error),
      CAI_OK);
  memset(&output, 0, sizeof(output));
  expect_int(state, "yolo_terminal_policy_bypass",
             cai_tool_registry_run(
                 registry, "exec_command",
                 "{\"cmd\":\"printf "
                 "forbidden\",\"workdir\":\"..\",\"yield_time_ms\":100}",
                 sink, &error),
             CAI_OK);
  expect_int(state, "yolo_no_policy_callback", policy_calls, 0);
  cai_tool_registry_destroy(registry);
  registry = NULL;
  expect_int(state, "yolo_invalid_registry",
             cai_tool_registry_new(&registry, &error), CAI_OK);
  read.root_path = outside;
  read.yolo = 1;
  expect_int(state, "yolo_invalid_read_base",
             cai_tool_registry_register_read_tool(registry, &read, &error),
             CAI_ERR_INVALID);
  terminal.root_path = outside;
  expect_int(
      state, "yolo_invalid_terminal_base",
      cai_tool_registry_register_terminal_tools(registry, &terminal, &error),
      CAI_ERR_INVALID);
  patch.root_path = outside;
  expect_int(
      state, "yolo_invalid_patch_base",
      cai_apply_patch(
          &patch,
          "*** Begin Patch\n*** Add File: invalid\n+bad\n*** End Patch\n", sink,
          &error),
      CAI_ERR_INVALID);
  cai_tool_registry_destroy(registry);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  unlink(added);
  unlink(nested);
  rmdir(missing);
  unlink(alias_added);
  unlink(shortcut);
  rmdir(target);
  unlink(linked);
  unlink(outside);
  rmdir(work);
  rmdir(root);
}
