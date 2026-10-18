#ifndef CAI_CLI_OPTIONS_H
#define CAI_CLI_OPTIONS_H

#include <stddef.h>
#include <stdio.h>

void cai_cli_print_help(FILE *out);

typedef struct cai_cli_options {
  const char *provider;
  const char *config_file;
  int no_config;
  int model_configured;
  const char *chrome_socket;
  const char *chrome_manifest;
  int chrome;
  int chrome_explicit;
  int web_search;
  int mcp;
  int todo;
  int searxng;
  int revgeo;
  int read_file, list_files, apply_patch, view_image, goals, skills;
  const char *searxng_url;
  const char *revgeo_url;
  int chrome_setup;
  int chrome_remove;
  const char *endpoint;
  const char *api_key_env;
  const char *model_metadata;
  const char *directory;
  const char *auth_json;
  const char *model;
  const char *reasoning_effort;
  const char *reasoning_summary;
  const char *service_tier;
  int model_explicit;
  int provider_explicit;
  int reasoning_effort_explicit;
  int reasoning_summary_explicit;
  const char *review_model;
  const char *review_reasoning_effort;
  const char *review_reasoning_summary;
  const char *agents_md;
  const char *config_dir;
  const char *skills_dir;
  const char *install_skill;
  int replace_skill;
  const char *identity;
  const char *developer_instructions;
  const char *goal;
  const char *base;
  const char *out;
  const char *output_type;
  const char *tool_output;
  const char *resume_id;
  const char *export_id;
  const char *export_dir;
  const char *import_file;
  const char *lockd;
  const char *lockd_client_pem;
  char *const *argv;
  int argc;
  size_t instruction_count;
  int login;
  int list;
  int resume_list;
  int new_session;
  int non_interactive;
  int review;
  int review_and_fix;
  int image_generation;
  int terminal;
  int shell_context;
  size_t shell_context_max_lines, shell_context_max_bytes;
  int review_subagent;
  int codex_agents_md;
  int verbosity;
  char diagnostic[512];
} cai_cli_options;

void cai_cli_options_init(cai_cli_options *options);
/* Returns 1 to start, 0 for help/version, -1 for an invalid command line. */
int cai_cli_parse_options(int argc, char *const *argv,
                          cai_cli_options *options);
int cai_cli_read_options(int argc, char *const *argv,
                         const cai_cli_options *defaults,
                         cai_cli_options *options);
int cai_cli_validate_options(cai_cli_options *options);
const char *cai_cli_instruction_at(const cai_cli_options *options,
                                   size_t index);

#endif
