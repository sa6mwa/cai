#ifndef CAI_APP_INTERNAL_H
#define CAI_APP_INTERNAL_H
#include <cai/app.h>
int cai_cli_app_main(int argc, char **argv, const cai_app_config *application);
size_t cai_cli_help_count(void);
const cai_app_option *cai_cli_help_at(size_t index);
int cai_cli_help_write(const char *program, cai_sink *sink, cai_error *error);
int cai_cli_help_print(const char *program);
int cai_cli_app_schema(cai_config **out, cai_error *error);
#endif
