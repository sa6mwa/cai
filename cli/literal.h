#ifndef CAI_CLI_LITERAL_H
#define CAI_CLI_LITERAL_H
#include <libmdf/mdf.h>

/* Render literal UTF-8 through libmdf's manual token stream, using bounded
 * chunks and preserving punctuation. The renderer must have a bound sink. */
int cai_cli_literal_render(mdf *renderer, const char *text);
#endif
