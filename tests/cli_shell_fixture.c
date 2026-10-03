#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
/* Exercise the production UI with a local Chat Completions provider. Only
 * protocol selection is replaced; execution, history and rendering are real. */
#include <cai/cai.h>

static int fixture_client_open(const cai_client_config *config,
                               cai_client **out, cai_error *error) {
  cai_client_config selected = *config;
  selected.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  return cai_client_open(&selected, out, error);
}
#define cai_client_open fixture_client_open
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main
#undef cai_client_open

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv); }
