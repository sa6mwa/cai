#ifndef CAI_CLI_THEME_H
#define CAI_CLI_THEME_H

#include <cai/cai.h>
#include <libmdf/mdf.h>
#include <softline/softline.h>

typedef struct cai_cli_theme {
  sl_prompt_theme_t prompt;
  const char *markdown;
  int ascii;
} cai_cli_theme;

/* Borrowed canonical names; NULL means success, otherwise a static diagnostic.
 */
const char *cai_cli_theme_parse(const char *text, cai_cli_theme *out);
void cai_cli_theme_markdown(const cai_cli_theme *theme, mdf_options *options);
int cai_cli_theme_list(cai_sink *sink, cai_error *error);

#endif
