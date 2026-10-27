#include "cai_process_environment.h"

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
