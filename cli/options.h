#ifndef CAI_CLI_OPTIONS_H
#define CAI_CLI_OPTIONS_H

#include <stddef.h>

typedef struct cai_cli_options {
  const char *provider;
  const char *endpoint;
  const char *api_key_env;
  const char *directory;
  const char *auth_json;
  const char *model;
  const char *reasoning_effort;
  const char *reasoning_summary;
  const char *review_model;
  const char *review_reasoning_effort;
  const char *review_reasoning_summary;
  const char *agents_md;
  const char *config_dir;
  const char *skills_dir;
  const char *identity;
  const char *developer_instructions;
  const char *goal;
  const char *base;
  const char *out;
  const char *output_type;
  const char *resume_id;
  char *const *argv;
  int argc;
  size_t instruction_count;
  int login;
  int new_session;
  int non_interactive;
  int review;
  int review_and_fix;
  int image_generation;
  int terminal;
  int review_subagent;
  int codex_agents_md;
  int verbosity;
} cai_cli_options;

void cai_cli_options_init(cai_cli_options *options);
/* Returns 1 to start, 0 for help/version, -1 for an invalid command line. */
int cai_cli_parse_options(int argc, char *const *argv,
                          cai_cli_options *options);
const char *cai_cli_instruction_at(const cai_cli_options *options,
                                   size_t index);

#endif
