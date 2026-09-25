#ifndef CAI_CLI_OPTIONS_H
#define CAI_CLI_OPTIONS_H

typedef struct cai_cli_options {
  const char *provider;
  const char *endpoint;
  const char *api_key_env;
  const char *workspace;
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
  const char *instructions;
  const char *resume_id;
  int login;
  int new_session;
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

#endif
