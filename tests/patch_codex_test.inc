/* Ordered matching, minimized from rejected journal calls and Codex's matcher.
 * Run through both public apply paths; no provider requests or sleeps. */
static void test_patch_codex_matching(test_state *state) {
  static const struct {
    const char *name, *before, *body, *after;
  } cases[] = {
      {"first_repeated", "same\nseparator\nsame\n", "@@\n-same\n+changed\n",
       "changed\nseparator\nsame\n"},
      {"ordered_repeated", "same\nsame\n",
       "@@\n-same\n+first\n@@\n-same\n+second\n", "first\nsecond\n"},
      {"implicit_first_hunk", "old\n", "-old\n+new\n", "new\n"},
      {"bare_blank_context", "head\n\nold\n", "@@\n head\n\n-old\n+new\n",
       "head\n\nnew\n"},
      {"trim_blank_context", "head\n\t\nold\n", "@@\n head\n \n-old\n+new\n",
       "head\n\t\nnew\n"},
      {"delete_every_line", "old\n", "@@\n-old\n", ""},
      /* The reported Slh36j call used context-only chunks between profiles. */
      {"profile_cursor", "2013:\nexempt: bus\ncurrent:\nexempt: bus\n",
       "@@\n 2013:\n@@\n-exempt: bus\n+exempt: bus\n@@\n current:\n"
       "@@\n-exempt: bus\n+exempt: heavy_bus\n",
       "2013:\nexempt: bus\ncurrent:\nexempt: heavy_bus\n"},
      {"exact_before_rstrip", "item  \nitem\n", "@@\n-item\n+changed\n",
       "item  \nchanged\n"},
      {"rstrip_before_trim", "  item\nitem \n", "@@\n-item\n+changed\n",
       "  item\nchanged\n"},
      {"trim_context_preserved", "  head \t\n  old \n end\t\n",
       "@@\n head\n-old\n+new\n end\n", "  head \t\nnew\n end\t\n"},
      {"internal_spacing_rejected", "head\nvalue  int\n",
       "@@\n head\n-value int\n+changed\n", NULL},
      {"full_lines_only", "prefixsame\n", "@@\n-same\n+changed\n", NULL},
      {"later_full_line", "prefixsame\nsame\n", "@@\n-same\n+changed\n",
       "prefixsame\nchanged\n"},
      {"anchor_exact_before_trim", "  section\nold\nsection\nold\n",
       "@@ section\n-old\n+new\n", "  section\nold\nsection\nnew\n"},
      {"unicode_punctuation", "“a”—‘b’ − x\n", "@@\n-\"a\"-'b' - x\n+changed\n",
       "changed\n"},
      {"unicode_internal_space", "a\302\240b\n", "@@\n-a b\n+changed\n",
       "changed\n"},
      {"unicode_edge_whitespace", "\302\205\341\232\200old\343\200\200\n",
       "@@\n-old\n+changed\n", "changed\n"},
      {"unicode_nonmapped_space", "a\342\200\200b\n", "@@\n-a b\n+changed\n",
       NULL},
      {"crlf", "head\r\nold\r\nend\r\n", "@@\n head\n-old\n+new\n end\n",
       "head\r\nnew\r\nend\r\n"},
      {"mixed_endings", "head\r\nold\nend\r", "@@\n head\n-old\n+new\n end\n",
       "head\r\nnew\r\nend\r"},
      {"cr_only", "head\rold\rend\r", "@@\n-old\n+new\n", "head\rnew\rend\r"},
      {"unterminated_last_line", "head\nold", "@@\n-old\n+new\n",
       "head\nnew\n"},
      {"empty_file_append", "", "@@\n+first\n", "first\n"},
      {"append_only", "old", "@@\n+last\n", "old\nlast\n"},
      {"append_with_anchor", "head\ntail\n", "@@ head\n+last\n",
       "head\ntail\nlast\n"},
      {"append_before_update", "head\ntail\n", "@@\n+last\n@@\n-head\n+HEAD\n",
       "HEAD\ntail\nlast\n"},
      {"two_appends", "head\n", "@@\n+first\n@@\n+second\n",
       "head\nfirst\nsecond\n"},
      {"eof_repeated", "same\nsame\n", "@@\n-same\n+last\n*** End of File\n",
       "same\nlast\n"},
      {"eof_mismatch", "first\nlast\n",
       "@@\n-first\n+changed\n*** End of File\n", NULL},
      {"trailing_empty_context", "head\nold\n", "@@\n-old\n+new\n \n",
       "head\nnew\n"},
      {"trailing_empty_context_unterminated", "head\nold",
       "@@\n-old\n+new\n \n", "head\nnew\n"},
      {"existing_final_blank", "old\n\n", "@@\n-old\n+new\n \n", "new\n\n"},
      {"too_many_lines", "one\n", "@@\n-one\n-two\n+new\n", NULL},
      {"second_hunk_atomicity", "old\n",
       "@@\n-old\n+new\n@@\n-missing\n+added\n", NULL},
      {"inserted_anchor_is_not_source", "old\ntail\n",
       "@@\n-old\n+anchor\n@@ anchor\n-tail\n+changed\n", NULL},
      {"deleted_original_cursor", "one\ntwo\nthree\n",
       "@@\n-one\n-two\n+first\n@@\n-three\n+last\n", "first\nlast\n"}};
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/cai-codex-patch-XXXXXX";
  char path[PATH_MAX], patch[4096], label[160];
  cai_patch_tool_config config;
  cai_tool_registry *registry = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  write_state writer;
  size_t i;
  int mode, rc;
  if (mkdtemp(root) == NULL) {
    test_fail(state, "patch_codex_setup", "mkdtemp failed");
    return;
  }
  memset(&config, 0, sizeof(config));
  config.root_path = root;
  snprintf(path, sizeof(path), "%s/case.txt", root);
  cai_error_init(&error);
  memset(&writer, 0, sizeof(writer));
  callbacks.write = test_write;
  callbacks.close = test_write_close;
  callbacks.context = &writer;
  expect_int(state, "patch_codex_sink",
             cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
  expect_int(state, "patch_codex_registry",
             cai_tool_registry_new(&registry, &error), CAI_OK);
  if (registry != NULL)
    expect_int(state, "patch_codex_register",
               cai_tool_registry_register_patch_tool(registry, &config, &error),
               CAI_OK);
  for (mode = 0; mode < 2 && sink != NULL && registry != NULL; mode++) {
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
      char *contents;
      write_file_or_die(path, cases[i].before);
      snprintf(patch, sizeof(patch),
               "*** Begin Patch\n*** Update File: case.txt\n%s*** End Patch\n",
               cases[i].body);
      cai_error_cleanup(&error);
      cai_error_init(&error);
      writer.length = 0U;
      writer.buffer[0] = '\0';
      rc = test_patch_invoke(state, &config, mode == 0 ? NULL : registry, patch,
                             sink, &error);
      snprintf(label, sizeof(label), "patch_codex_%s_%d", cases[i].name, mode);
      expect_int(state, label, rc,
                 cases[i].after != NULL ? CAI_OK : CAI_ERR_INVALID);
      contents = read_file_or_die(path);
      expect_str(state, label, contents,
                 cases[i].after != NULL ? cases[i].after : cases[i].before);
      expect_int(state, "patch_codex_receipt_after_success",
                 writer.length != 0U, cases[i].after != NULL);
      free(contents);
    }
  }
  cai_tool_registry_destroy(registry);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  unlink(path);
  rmdir(root);
}
