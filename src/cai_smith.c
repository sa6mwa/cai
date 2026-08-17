#include <cai/smith.h>
#include <cai/tools/patch.h>
#include <cai/tools/read.h>

#include "cai_internal.h"

#include <string.h>

static const char cai_smith_prompt_prefix[] = "You are ";
static const char *const cai_smith_prompt_suffix_parts[] = {
    ", a coding agent running in CAI agent mode. CAI is an open source "
    "coding-agent harness. You are expected to be precise, safe, and "
    "helpful.\n\n",
    "Your currently configured local tools are read_file, list_files, and "
    "apply_patch. "
    "Use them to inspect the workspace before making claims about its "
    "contents. Command execution, image generation, and terminal management "
    "are not available in this Smith profile stage. Do not imply that an "
    "unavailable tool was run or that a change was made.\n\n",
    "Communicate concisely and directly. State assumptions, risks, and "
    "verification results clearly. Follow repository instructions supplied "
    "by the host when they are available.\n"};

void cai_smith_config_init(cai_smith_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

const char *cai_smith_prompt_version(void) { return CAI_SMITH_PROMPT_VERSION; }

static int cai_smith_render_instructions(const cai_allocator *allocator,
                                         const cai_smith_config *config,
                                         char **out, cai_error *error) {
  const char *identity;
  const char *extension;
  size_t prefix_length;
  size_t identity_length;
  size_t suffix_length;
  size_t extension_length;
  size_t total_length;
  size_t offset;
  size_t i;
  char *instructions;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith instruction output pointer is required");
  }
  *out = NULL;
  identity = config->agent_identity != NULL ? config->agent_identity
                                            : CAI_SMITH_DEFAULT_IDENTITY;
  extension = config->developer_instructions_extension;
  if (identity[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith agent identity must not be empty");
  }
  prefix_length = strlen(cai_smith_prompt_prefix);
  identity_length = strlen(identity);
  suffix_length = 0U;
  for (i = 0U; i < sizeof(cai_smith_prompt_suffix_parts) /
                       sizeof(cai_smith_prompt_suffix_parts[0]);
       i++) {
    suffix_length += strlen(cai_smith_prompt_suffix_parts[i]);
  }
  extension_length = extension != NULL ? strlen(extension) : 0U;
  total_length = prefix_length + identity_length + suffix_length;
  if (extension_length > 0U) {
    total_length += 2U + extension_length;
  }
  instructions = (char *)cai_alloc(allocator, total_length + 1U);
  if (instructions == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate Smith instructions");
  }
  memcpy(instructions, cai_smith_prompt_prefix, prefix_length);
  memcpy(instructions + prefix_length, identity, identity_length);
  offset = prefix_length + identity_length;
  for (i = 0U; i < sizeof(cai_smith_prompt_suffix_parts) /
                       sizeof(cai_smith_prompt_suffix_parts[0]);
       i++) {
    size_t part_length;

    part_length = strlen(cai_smith_prompt_suffix_parts[i]);
    memcpy(instructions + offset, cai_smith_prompt_suffix_parts[i],
           part_length);
    offset += part_length;
  }
  if (extension_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    memcpy(instructions + offset + 2U, extension, extension_length);
  }
  instructions[total_length] = '\0';
  *out = instructions;
  return CAI_OK;
}

int cai_client_new_smith_agent(cai_client *client,
                               const cai_smith_config *config, cai_agent **out,
                               cai_error *error) {
  cai_client_impl *client_impl;
  cai_agent_config agent_config;
  cai_read_tool_config read_config;
  cai_patch_tool_config patch_config;
  char *instructions;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith agent output pointer is required");
  }
  *out = NULL;
  if (client == NULL || config == NULL || config->workspace_directory == NULL ||
      config->workspace_directory[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith client and workspace directory are required");
  }
  client_impl = CAI_CLIENT_IMPL(client);
  if (client_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is closed");
  }
  instructions = NULL;
  rc = cai_smith_render_instructions(&client_impl->allocator, config,
                                     &instructions, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cai_agent_config_init(&agent_config);
  agent_config.model =
      config->model != NULL ? config->model : CAI_SMITH_DEFAULT_MODEL;
  agent_config.developer_instructions = instructions;
  agent_config.reasoning_effort = config->reasoning_effort != NULL
                                      ? config->reasoning_effort
                                      : CAI_REASONING_EFFORT_MEDIUM;
  agent_config.tool_choice = CAI_TOOL_CHOICE_AUTO;
  agent_config.disable_parallel_tool_calls = 1;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  agent_config.enable_local_history = 1;
  agent_config.disable_auto_compaction = 1;
  rc = cai_client_new_agent(client, &agent_config, out, error);
  cai_free_mem(&client_impl->allocator, instructions);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&read_config, 0, sizeof(read_config));
  read_config.root_path = config->workspace_directory;
  read_config.default_workdir = config->workspace_directory;
  read_config.content_memory_limit = config->file_content_memory_limit;
  read_config.content_max_bytes = config->file_content_max_bytes;
  read_config.content_spool_dir = config->file_content_spool_dir;
  rc = cai_agent_register_read_tool(*out, &read_config, error);
  if (rc == CAI_OK) {
    rc = cai_agent_register_list_files_tool(*out, &read_config, error);
  }
  if (rc == CAI_OK) {
    memset(&patch_config, 0, sizeof(patch_config));
    patch_config.root_path = config->workspace_directory;
    rc = cai_agent_register_patch_tool(*out, &patch_config, error);
  }
  if (rc != CAI_OK) {
    cai_agent_destroy(*out);
    *out = NULL;
  }
  return rc;
}
