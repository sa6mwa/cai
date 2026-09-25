#ifndef CAI_CLI_REVIEW_REPORT_H
#define CAI_CLI_REVIEW_REPORT_H

#include <stddef.h>

/* Format one libcai-validated review report. Caller releases *out with free. */
int cai_cli_review_format(const char *json, size_t length,
                          const char *output_type, char **out);

#endif
