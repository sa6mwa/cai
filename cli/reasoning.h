#ifndef CAI_CLI_REASONING_H
#define CAI_CLI_REASONING_H
#include <stddef.h>

/* Only a bounded heading and leading indentation are retained. Body bytes
 * flow directly to the callback once the first nonblank body line starts. */
typedef struct cai_cli_reasoning {
  char header[512];
  size_t header_length;
  char title[512];
  char indent[512];
  size_t indent_length;
  int started;
  int phase;
  int skip_lf;
  int dirty;
} cai_cli_reasoning;
typedef int (*cai_cli_reasoning_body_fn)(void *context, const char *data,
                                         size_t length);
/* Prefix Markdown source lines incrementally. CRLF and CR become one LF;
 * only line-boundary state is retained, never the reasoning body. */
typedef struct cai_cli_reasoning_quote {
  int line_start;
  int skip_lf;
} cai_cli_reasoning_quote;
void cai_cli_reasoning_quote_init(cai_cli_reasoning_quote *quote);
int cai_cli_reasoning_quote_feed(cai_cli_reasoning_quote *quote,
                                 const char *data, size_t length,
                                 cai_cli_reasoning_body_fn write,
                                 void *context);
void cai_cli_reasoning_reset(cai_cli_reasoning *reasoning);
/* End a provider section, retaining its title until the next section starts. */
void cai_cli_reasoning_end(cai_cli_reasoning *reasoning);
int cai_cli_reasoning_feed(cai_cli_reasoning *reasoning, const char *data,
                           size_t length, cai_cli_reasoning_body_fn body,
                           void *context);
/* Render the bounded Markdown heading as plain UTF-8 for softline's message. */
int cai_cli_reasoning_title(cai_cli_reasoning *reasoning, const char **out);
#endif
