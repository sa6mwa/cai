#include "../cli/theme.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct capture {
  char bytes[16384];
  size_t length;
  int fail;
} capture;

static int write_capture(void *context, const void *bytes, size_t length,
                         cai_error *error) {
  capture *out = context;
  (void)error;
  if (out->fail || length >= sizeof(out->bytes) - out->length)
    return CAI_ERR_TRANSPORT;
  memcpy(out->bytes + out->length, bytes, length);
  out->length += length;
  out->bytes[out->length] = '\0';
  return CAI_OK;
}

int main(void) {
  static const struct {
    const char *alias, *markdown;
    sl_prompt_theme_t prompt;
  } pairs[] = {{"synthwave", "synthwave-84", SL_PROMPT_THEME_SYNTHWAVE},
               {"plain", "ascii", SL_PROMPT_THEME_PLAIN},
               {"accent", "night-owl", SL_PROMPT_THEME_ACCENT},
               {"dracula", "dracula", SL_PROMPT_THEME_DRACULA},
               {"gruvbox", "gruvbox", SL_PROMPT_THEME_GRUVBOX},
               {"monochrome", "horizon", SL_PROMPT_THEME_MONOCHROME},
               {"monogreen", "everforest", SL_PROMPT_THEME_MONOGREEN},
               {"outrun", "outrun-electric", SL_PROMPT_THEME_OUTRUN},
               {"riced", "catppuccin-mocha", SL_PROMPT_THEME_RICED},
               {"default", "default", SL_PROMPT_THEME_DEFAULT}};
  static const char *const invalid[] = {"",
                                        ":dracula",
                                        "riced:",
                                        ":",
                                        "riced:dracula:default",
                                        "unknown",
                                        "unknown:dracula",
                                        "riced:unknown",
                                        "dracula ",
                                        " dracula"};
  cai_cli_theme theme, explicit_theme;
  mdf_options options;
  cai_error error;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  capture out;
  char pair[128];
  size_t i;
  for (i = 0U; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
    assert(cai_cli_theme_parse(pairs[i].alias, &theme) == NULL);
    assert(theme.prompt == pairs[i].prompt);
    assert(strcmp(cai_cli_theme_prompt_name(&theme), pairs[i].alias) == 0);
    assert(theme.ascii == (strcmp(pairs[i].markdown, "ascii") == 0));
    if (!theme.ascii)
      assert(strcmp(theme.markdown, pairs[i].markdown) == 0);
    snprintf(pair, sizeof(pair), "%s:%s", pairs[i].alias, pairs[i].markdown);
    assert(cai_cli_theme_parse(pair, &explicit_theme) == NULL);
    assert(theme.prompt == explicit_theme.prompt &&
           theme.ascii == explicit_theme.ascii);
    if (!theme.ascii)
      assert(strcmp(theme.markdown, explicit_theme.markdown) == 0);
    mdf_options_init(&options);
    cai_cli_theme_markdown(&theme, &options);
    assert(options.theme_name == theme.markdown);
    if (theme.ascii)
      assert(options.ansi_mode == MDF_ANSI_OFF);
  }
  assert(cai_cli_theme_parse("RICED:IOSVKEM", &theme) == NULL);
  assert(theme.prompt == SL_PROMPT_THEME_RICED &&
         strcmp(theme.markdown, "iosvkem") == 0);
  assert(cai_cli_theme_parse(NULL, &theme) == NULL &&
         theme.prompt == SL_PROMPT_THEME_DEFAULT);
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    explicit_theme = theme;
    assert(cai_cli_theme_parse(invalid[i], &theme) != NULL);
    assert(theme.prompt == explicit_theme.prompt &&
           theme.markdown == explicit_theme.markdown);
  }
  cai_error_init(&error);
  memset(&out, 0, sizeof(out));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = &out;
  callbacks.write = write_capture;
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  assert(cai_cli_theme_list(sink, &error) == CAI_OK);
  for (i = 0U; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
    snprintf(pair, sizeof(pair), "%s:%s", pairs[i].alias, pairs[i].markdown);
    assert(strstr(out.bytes, pair) != NULL);
  }
  assert(strstr(out.bytes,
                "ascii (escape-free UTF-8 Markdown, not a color theme)") !=
         NULL);
  for (i = 0U; i < mdf_theme_count(); i++) {
    snprintf(pair, sizeof(pair), "  %s\n", mdf_theme_name(i));
    assert(strstr(strstr(out.bytes, "libmdf themes:\n"), pair) != NULL);
  }
  out.length = 0U;
  assert(cai_cli_theme_parse("riced:iosvkem", &theme) == NULL);
  assert(cai_cli_theme_describe(&theme, sink, &error) == CAI_OK);
  assert(strstr(out.bytes, "Current: `riced:iosvkem`") != NULL);
  for (i = 0U; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
    snprintf(pair, sizeof(pair), "`%s:%s`", pairs[i].alias, pairs[i].markdown);
    assert(strstr(out.bytes, pair) != NULL);
  }
  for (i = 0U; i < mdf_theme_count(); i++) {
    snprintf(pair, sizeof(pair), "`%s`", mdf_theme_name(i));
    assert(strstr(strstr(out.bytes, "### Libmdf themes\n"), pair) != NULL);
  }
  out.length = 0U;
  assert(cai_cli_theme_parse("dracula:ascii", &theme) == NULL);
  assert(cai_cli_theme_describe(&theme, sink, &error) == CAI_OK);
  assert(strstr(out.bytes, "Current: `dracula:ascii`") != NULL);
  out.fail = 1;
  assert(cai_cli_theme_list(sink, &error) == CAI_ERR_TRANSPORT);
  assert(cai_cli_theme_describe(&theme, sink, &error) == CAI_ERR_TRANSPORT);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  return 0;
}
