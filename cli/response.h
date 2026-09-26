#ifndef CAI_CLI_RESPONSE_H
#define CAI_CLI_RESPONSE_H

#include <cai/cai.h>
#include <stdio.h>

/* Only the latest top-level response is retained, in an unlinked private file.
 * Activity logging remains immediate; this is file-backed final-output state.
 */
typedef struct cai_cli_response {
  FILE *file;
  int next_response;
} cai_cli_response;

int cai_cli_response_append(cai_cli_response *response, const char *directory,
                            const char *bytes, size_t count, cai_error *error);
void cai_cli_response_boundary(cai_cli_response *response);
int cai_cli_response_source(cai_cli_response *response, cai_source **out,
                            cai_error *error);
void cai_cli_response_close(cai_cli_response *response);

#endif
