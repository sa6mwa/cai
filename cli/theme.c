#include "theme.h"
#include <stdio.h>
#include <string.h>

static const struct {
  const char *name;
  sl_prompt_theme_t prompt;
  const char *markdown;
} themes[] = {{"default", SL_PROMPT_THEME_DEFAULT, "default"},
              {"plain", SL_PROMPT_THEME_PLAIN, "ascii"},
              {"accent", SL_PROMPT_THEME_ACCENT, "night-owl"},
              {"dracula", SL_PROMPT_THEME_DRACULA, "dracula"},
              {"gruvbox", SL_PROMPT_THEME_GRUVBOX, "gruvbox"},
              {"monochrome", SL_PROMPT_THEME_MONOCHROME, "horizon"},
              {"monogreen", SL_PROMPT_THEME_MONOGREEN, "everforest"},
              {"outrun", SL_PROMPT_THEME_OUTRUN, "outrun-electric"},
              {"riced", SL_PROMPT_THEME_RICED, "catppuccin-mocha"},
              {"synthwave", SL_PROMPT_THEME_SYNTHWAVE, "synthwave-84"}};

static int theme_equal(const char *a, size_t length, const char *b) {
  size_t i;
  if (strlen(b) != length)
    return 0;
  for (i = 0U; i < length; i++) {
    unsigned char ch = (unsigned char)a[i];
    if (ch >= 'A' && ch <= 'Z')
      ch = (unsigned char)(ch - 'A' + 'a');
    if (ch != (unsigned char)b[i])
      return 0;
  }
  return 1;
}

const char *cai_cli_theme_parse(const char *text, cai_cli_theme *out) {
  const char *separator, *markdown;
  size_t length, i;
  cai_cli_theme selected;
  if (text == NULL)
    text = "default";
  separator = strchr(text, ':');
  length = separator != NULL ? (size_t)(separator - text) : strlen(text);
  if (length == 0U || (separator != NULL && separator[1] == '\0'))
    return "theme requires a softline name or SOFTLINE:LIBMDF; use "
           "--list-themes";
  if (separator != NULL && strchr(separator + 1U, ':') != NULL)
    return "theme must contain at most one ':'; use SOFTLINE:LIBMDF";
  for (i = 0U; i < sizeof(themes) / sizeof(themes[0]); i++)
    if (theme_equal(text, length, themes[i].name))
      break;
  if (i == sizeof(themes) / sizeof(themes[0]))
    return "unknown softline theme; use --list-themes";
  selected.prompt = themes[i].prompt;
  markdown = separator != NULL ? separator + 1U : themes[i].markdown;
  selected.ascii = theme_equal(markdown, strlen(markdown), "ascii");
  selected.markdown = NULL;
  if (!selected.ascii) {
    for (i = 0U; i < mdf_theme_count(); i++) {
      const char *name = mdf_theme_name(i);
      if (theme_equal(markdown, strlen(markdown), name)) {
        selected.markdown = name;
        break;
      }
    }
    if (selected.markdown == NULL)
      return "unknown libmdf theme; use --list-themes";
  }
  *out = selected;
  return NULL;
}

void cai_cli_theme_markdown(const cai_cli_theme *theme, mdf_options *options) {
  options->theme_name = theme->markdown;
  if (theme->ascii)
    options->ansi_mode = MDF_ANSI_OFF;
}

int cai_cli_theme_list(cai_sink *sink, cai_error *error) {
  static const char heading[] = "Softline themes and default libmdf pairings:\n"
                                "  NAME         SOFTLINE:LIBMDF\n";
  static const char middle[] = "\nlibmdf themes:\n";
  static const char ending[] =
      "  ascii (escape-free UTF-8 Markdown, not a color theme)\n\n"
      "Use --theme NAME for a pairing, or --theme SOFTLINE:LIBMDF.\n";
  char row[256];
  size_t i;
  int rc = cai_sink_write(sink, heading, sizeof(heading) - 1U, error);
  for (i = 0U; rc == CAI_OK && i < sizeof(themes) / sizeof(themes[0]); i++) {
    int n = snprintf(row, sizeof(row), "  %-12s %s:%s\n", themes[i].name,
                     themes[i].name, themes[i].markdown);
    rc = cai_sink_write(sink, row, (size_t)n, error);
  }
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, middle, sizeof(middle) - 1U, error);
  for (i = 0U; rc == CAI_OK && i < mdf_theme_count(); i++) {
    const char *name = mdf_theme_name(i);
    rc = cai_sink_write(sink, "  ", 2U, error);
    if (rc == CAI_OK)
      rc = cai_sink_write(sink, name, strlen(name), error);
    if (rc == CAI_OK)
      rc = cai_sink_write(sink, "\n", 1U, error);
  }
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, ending, sizeof(ending) - 1U, error);
  return rc;
}
