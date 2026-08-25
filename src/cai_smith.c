#include <cai/smith.h>
#include <cai/tools/patch.h>
#include <cai/tools/read.h>
#include <cai/tools/terminal.h>
#include <cai/tools/view_image.h>

#include "cai_internal.h"
#include "cai_smith_gpt_5_6_prompt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char *realpath(const char *path, char *resolved_path);

#define CAI_SMITH_MAX_REPOSITORY_INSTRUCTIONS (128U * 1024U)
#define CAI_AGENT_IDENTITY_TOKEN "{{agent_identity}}"
#define CAI_AGENT_TOOLS_TOKEN "{{agent_tools}}"

void cai_smith_config_init(cai_smith_config *config) {
  cai_agent_preset_config_init(config);
}

void cai_agent_preset_config_init(cai_agent_preset_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

void cai_agent_preset_from_smith(cai_agent_preset *preset) {
  if (preset == NULL) {
    return;
  }
  memset(preset, 0, sizeof(*preset));
  preset->name = CAI_SMITH_PRESET;
  preset->prompt_version = CAI_SMITH_PROMPT_VERSION;
  preset->default_identity = CAI_SMITH_DEFAULT_IDENTITY;
  preset->default_model = CAI_SMITH_DEFAULT_MODEL;
  preset->default_reasoning_effort = CAI_REASONING_EFFORT_MEDIUM;
  preset->default_reasoning_summary = CAI_REASONING_SUMMARY_AUTO;
  preset->tool_capabilities =
      CAI_AGENT_PRESET_TOOL_READ_FILE | CAI_AGENT_PRESET_TOOL_LIST_FILES |
      CAI_AGENT_PRESET_TOOL_APPLY_PATCH | CAI_AGENT_PRESET_TOOL_TERMINAL |
      CAI_AGENT_PRESET_TOOL_VIEW_IMAGE | CAI_AGENT_PRESET_TOOL_GOAL |
      CAI_AGENT_PRESET_TOOL_MCP | CAI_AGENT_PRESET_TOOL_IMAGE_GENERATION |
      CAI_AGENT_PRESET_TOOL_SKILLS | CAI_AGENT_PRESET_TOOL_SUBAGENTS;
  preset->review_tool_capabilities =
      CAI_AGENT_PRESET_TOOL_READ_FILE | CAI_AGENT_PRESET_TOOL_LIST_FILES |
      CAI_AGENT_PRESET_TOOL_TERMINAL | CAI_AGENT_PRESET_TOOL_VIEW_IMAGE |
      CAI_AGENT_PRESET_TOOL_SKILLS;
  preset->supports_review = 1;
}

const char *cai_smith_prompt_version(void) { return CAI_SMITH_PROMPT_VERSION; }

static int cai_preset_render_template(
    const cai_allocator *allocator, const char *template_text,
    const cai_agent_preset *preset, const cai_agent_preset_config *config,
    const char *repository_instructions, const char *skill_catalog,
    const char *tool_contract, char **out, cai_error *error) {
  const char *identity;
  const char *extension;
  const char *cursor;
  const char *identity_token;
  const char *tools_token;
  const char *token;
  const char *replacement;
  size_t identity_length;
  size_t tool_contract_length;
  size_t repository_length;
  size_t skill_catalog_length;
  size_t extension_length;
  size_t total_length;
  size_t part_length;
  size_t prefix_length;
  size_t offset;
  char *instructions;

  *out = NULL;
  identity = config->agent_identity != NULL ? config->agent_identity
                                            : preset->default_identity;
  extension = config->developer_instructions_extension;
  if (identity == NULL || identity[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent preset identity must not be empty");
  }
  identity_length = strlen(identity);
  tool_contract_length = tool_contract != NULL ? strlen(tool_contract) : 0U;
  repository_length =
      repository_instructions != NULL ? strlen(repository_instructions) : 0U;
  skill_catalog_length = skill_catalog != NULL ? strlen(skill_catalog) : 0U;
  extension_length = extension != NULL ? strlen(extension) : 0U;
  total_length = 0U;
  cursor = template_text;
  for (;;) {
    identity_token = strstr(cursor, CAI_AGENT_IDENTITY_TOKEN);
    tools_token =
        tool_contract != NULL ? strstr(cursor, CAI_AGENT_TOOLS_TOKEN) : NULL;
    if (identity_token == NULL && tools_token == NULL) {
      part_length = strlen(cursor);
      if (part_length > SIZE_MAX - total_length) {
        return cai_set_error(error, CAI_ERR_INVALID,
                             "agent preset instructions are too large");
      }
      total_length += part_length;
      break;
    }
    if (tools_token == NULL ||
        (identity_token != NULL && identity_token < tools_token)) {
      token = identity_token;
      replacement = identity;
      part_length = identity_length;
    } else {
      token = tools_token;
      replacement = tool_contract;
      part_length = tool_contract_length;
    }
    if ((size_t)(token - cursor) > SIZE_MAX - total_length ||
        part_length > SIZE_MAX - total_length - (size_t)(token - cursor)) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "agent preset instructions are too large");
    }
    total_length += (size_t)(token - cursor) + part_length;
    cursor =
        token + (replacement == identity ? sizeof(CAI_AGENT_IDENTITY_TOKEN) - 1U
                                         : sizeof(CAI_AGENT_TOOLS_TOKEN) - 1U);
  }
  if (repository_length > 0U) {
    if (total_length > SIZE_MAX - 2U - repository_length) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "agent preset instructions are too large");
    }
    total_length += 2U + repository_length;
  }
  if (skill_catalog_length > 0U) {
    if (total_length > SIZE_MAX - 2U - skill_catalog_length) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "agent preset instructions are too large");
    }
    total_length += 2U + skill_catalog_length;
  }
  if (extension_length > 0U) {
    if (total_length > SIZE_MAX - 2U - extension_length) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "agent preset instructions are too large");
    }
    total_length += 2U + extension_length;
  }
  instructions = (char *)cai_alloc(allocator, total_length + 1U);
  if (instructions == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent preset instructions");
  }
  cursor = template_text;
  offset = 0U;
  for (;;) {
    identity_token = strstr(cursor, CAI_AGENT_IDENTITY_TOKEN);
    tools_token =
        tool_contract != NULL ? strstr(cursor, CAI_AGENT_TOOLS_TOKEN) : NULL;
    if (identity_token == NULL && tools_token == NULL) {
      part_length = strlen(cursor);
      memcpy(instructions + offset, cursor, part_length);
      offset += part_length;
      break;
    }
    if (tools_token == NULL ||
        (identity_token != NULL && identity_token < tools_token)) {
      token = identity_token;
      replacement = identity;
      part_length = identity_length;
    } else {
      token = tools_token;
      replacement = tool_contract;
      part_length = tool_contract_length;
    }
    prefix_length = (size_t)(token - cursor);
    memcpy(instructions + offset, cursor, prefix_length);
    offset += prefix_length;
    memcpy(instructions + offset, replacement, part_length);
    offset += part_length;
    cursor =
        token + (replacement == identity ? sizeof(CAI_AGENT_IDENTITY_TOKEN) - 1U
                                         : sizeof(CAI_AGENT_TOOLS_TOKEN) - 1U);
  }
  if (repository_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    offset += 2U;
    memcpy(instructions + offset, repository_instructions, repository_length);
    offset += repository_length;
  }
  if (skill_catalog_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    offset += 2U;
    memcpy(instructions + offset, skill_catalog, skill_catalog_length);
    offset += skill_catalog_length;
  }
  if (extension_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    offset += 2U;
    memcpy(instructions + offset, extension, extension_length);
    offset += extension_length;
  }
  instructions[offset] = '\0';
  *out = instructions;
  return CAI_OK;
}

static int cai_smith_build_tool_contract(const cai_allocator *allocator,
                                         unsigned long capabilities, char **out,
                                         cai_error *error) {
  static const char *const environment_heading =
      "# CAI agent-mode capability adjustments\n\n";
  static const char *const environment_communication =
      "The preceding GPT-5.6 Codex instructions are the default contract. "
      "This section supersedes them only where CAI exposes a different "
      "capability or host interface. CAI has no commentary/final channels: "
      "use ordinary assistant messages and tool calls. Before a relevant tool "
      "call, send one brief visible progress preamble when the host renders "
      "output; make the final response self-contained.\n\n";
  static const char *const environment_tools =
      "Use only the tools present in this request and follow their schemas. "
      "CAI dispatches calls serially, so do not request parallel calls. CAI "
      "has no update_plan tool; goals are durable session state, not a task "
      "plan. Repository policies supplied by the host are binding for the "
      "workspace paths they govern; more-specific policy wins.\n\n";
  static const char *const common =
      "Never invent an unavailable tool, emulate its output, or claim that an "
      "action or verification occurred when it did not. ";
  static const char *const read_file =
      "Use read_file to inspect files before asserting their contents. ";
  static const char *const list_files =
      "Use list_files to inspect workspace paths. ";
  static const char *const patch =
      "Make workspace edits only with apply_patch. ";
  static const char *const terminal =
      "Use exec_command for one managed terminal command and write_stdin only "
      "with its returned session ID to supply input, wait, or terminate it. ";
  static const char *const view_image =
      "Use view_image to inspect local images when it is present. ";
  static const char *const goal =
      "Create a goal only when explicitly requested. Mark it complete only "
      "when its objective is achieved; mark it blocked only after the same "
      "external blocker recurs for three consecutive goal turns. Use "
      "clear_goal to remove a goal without claiming completion. ";
  static const char *const mcp = "The host may expose configured MCP tools. ";
  static const char *const image_generation =
      "The host may expose image_generation when it enables that capability. ";
  static const char *const skills =
      "CAI skills are only the globally configured packages listed in the "
      "developer instructions. Select one when the user's request names it or "
      "clearly matches its description, then use read_skill for its complete "
      "SKILL.md and permitted package-relative resources. Do not assume Codex "
      "plugins, external skill providers, or other undisclosed skill APIs. ";
  static const char *const subagents =
      "Use run_subagent only for an enabled delegated role; it runs one "
      "isolated child synchronously and returns its durable handover. ";
  static const char *const serial =
      "Tool calls are serial: complete and assess one call before issuing "
      "another.";
  const char *parts[15];
  size_t count;
  size_t length;
  size_t i;
  size_t part_length;
  char *contract;

  *out = NULL;
  count = 0U;
  parts[count++] = environment_heading;
  parts[count++] = environment_communication;
  parts[count++] = environment_tools;
  parts[count++] = common;
  if ((capabilities & CAI_AGENT_PRESET_TOOL_READ_FILE) != 0UL) {
    parts[count++] = read_file;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_LIST_FILES) != 0UL) {
    parts[count++] = list_files;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_APPLY_PATCH) != 0UL) {
    parts[count++] = patch;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_TERMINAL) != 0UL) {
    parts[count++] = terminal;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_VIEW_IMAGE) != 0UL) {
    parts[count++] = view_image;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_GOAL) != 0UL) {
    parts[count++] = goal;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_MCP) != 0UL) {
    parts[count++] = mcp;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_IMAGE_GENERATION) != 0UL) {
    parts[count++] = image_generation;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_SKILLS) != 0UL) {
    parts[count++] = skills;
  }
  if ((capabilities & CAI_AGENT_PRESET_TOOL_SUBAGENTS) != 0UL) {
    parts[count++] = subagents;
  }
  parts[count++] = serial;
  length = 0U;
  for (i = 0U; i < count; i++) {
    part_length = strlen(parts[i]);
    if (part_length > SIZE_MAX - length) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "agent tool contract is too large");
    }
    length += part_length;
  }
  contract = (char *)cai_alloc(allocator, length + 1U);
  if (contract == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent tool contract");
  }
  length = 0U;
  for (i = 0U; i < count; i++) {
    part_length = strlen(parts[i]);
    memcpy(contract + length, parts[i], part_length);
    length += part_length;
  }
  contract[length] = '\0';
  *out = contract;
  return CAI_OK;
}

static int cai_smith_join_prompt_parts(const cai_allocator *allocator,
                                       const char *const *parts, char **out,
                                       cai_error *error) {
  size_t length;
  size_t offset;
  size_t i;
  size_t part_length;
  char *joined;

  *out = NULL;
  length = 0U;
  for (i = 0U; parts[i] != NULL; i++) {
    part_length = strlen(parts[i]);
    if (part_length > SIZE_MAX - length) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "agent prompt template is too large");
    }
    length += part_length;
  }
  joined = (char *)cai_alloc(allocator, length + 1U);
  if (joined == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent prompt template");
  }
  offset = 0U;
  for (i = 0U; parts[i] != NULL; i++) {
    part_length = strlen(parts[i]);
    memcpy(joined + offset, parts[i], part_length);
    offset += part_length;
  }
  joined[offset] = '\0';
  *out = joined;
  return CAI_OK;
}

static int cai_smith_remove_prompt_fragment(char *prompt, const char *fragment,
                                            cai_error *error) {
  char *match;
  size_t fragment_length;

  match = strstr(prompt, fragment);
  if (match == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "pinned Codex prompt fragment is missing");
  }
  fragment_length = strlen(fragment);
  memmove(match, match + fragment_length, strlen(match + fragment_length) + 1U);
  return CAI_OK;
}

static int cai_smith_render_instructions(const cai_allocator *allocator,
                                         const cai_agent_preset *preset,
                                         const cai_agent_preset_config *config,
                                         unsigned long tool_capabilities,
                                         const char *repository_instructions,
                                         const char *skill_catalog, char **out,
                                         cai_error *error) {
  char *tool_contract;
  char *template_text;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith instruction output pointer is required");
  }
  *out = NULL;
  if (preset->developer_instructions != NULL) {
    return cai_preset_render_template(allocator, preset->developer_instructions,
                                      preset, config, repository_instructions,
                                      skill_catalog, NULL, out, error);
  }
  tool_contract = NULL;
  template_text = NULL;
  rc = cai_smith_join_prompt_parts(allocator, cai_smith_gpt_5_6_prompt_parts,
                                   &template_text, error);
  if (rc == CAI_OK &&
      (tool_capabilities & CAI_AGENT_PRESET_TOOL_APPLY_PATCH) == 0UL) {
    rc = cai_smith_remove_prompt_fragment(
        template_text,
        "Use `apply_patch` for local file edits. Do not create or edit files "
        "with `cat` or other shell write tricks. Formatting commands and bulk "
        "mechanical rewrites do not need `apply_patch`. Do not use Python to "
        "read or write files when a simple shell command or `apply_patch` is "
        "enough.\n",
        error);
  }
  if (rc == CAI_OK &&
      (tool_capabilities & CAI_AGENT_PRESET_TOOL_TERMINAL) == 0UL) {
    rc = cai_smith_remove_prompt_fragment(
        template_text,
        "- Exercise caution when escaping text for exec_command calls - "
        "backticks and `$()` passed to the `cmd` argument will still execute. "
        "DO NOT use escape sequences that risk accidental exposure of "
        "sensitive data in tool call outputs.\n",
        error);
  }
  if (rc == CAI_OK) {
    rc = cai_smith_build_tool_contract(allocator, tool_capabilities,
                                       &tool_contract, error);
  }
  if (rc == CAI_OK) {
    rc = cai_preset_render_template(allocator, template_text, preset, config,
                                    repository_instructions, skill_catalog,
                                    tool_contract, out, error);
  }
  cai_free_mem(allocator, template_text);
  cai_free_mem(allocator, tool_contract);
  return rc;
}

static int cai_smith_read_instructions_fd(const cai_allocator *allocator,
                                          int instructions_fd, char **out,
                                          cai_error *error) {
  struct stat st;
  size_t length;
  size_t offset;
  ssize_t nread;
  *out = NULL;
  if (fstat(instructions_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_nlink != 1) {
    (void)close(instructions_fd);
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "repository instructions must be a private regular file");
  }
  if (st.st_size < 0 ||
      (unsigned long long)st.st_size >
          (unsigned long long)CAI_SMITH_MAX_REPOSITORY_INSTRUCTIONS) {
    (void)close(instructions_fd);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "repository instructions exceed Smith limit");
  }
  length = (size_t)st.st_size;
  *out = (char *)cai_alloc(allocator, length + 1U);
  if (*out == NULL) {
    (void)close(instructions_fd);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate repository instructions");
  }
  offset = 0U;
  while (offset < length) {
    nread = read(instructions_fd, *out + offset, length - offset);
    if (nread > 0) {
      offset += (size_t)nread;
    } else if (nread < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  (void)close(instructions_fd);
  if (offset != length) {
    cai_free_mem(allocator, *out);
    *out = NULL;
    return cai_set_error(error, CAI_ERR_INVALID,
                         "failed to read repository instructions");
  }
  (*out)[length] = '\0';
  return CAI_OK;
}

static int
cai_smith_load_workspace_instruction_file(const cai_allocator *allocator,
                                          const char *workspace, char **out,
                                          cai_error *error) {
  int workspace_fd;
  int instructions_fd;
  int rc;

  *out = NULL;
  workspace_fd = open(workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (workspace_fd < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "failed to open Smith workspace");
  }
  instructions_fd =
      openat(workspace_fd, "AGENTS.md", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  (void)close(workspace_fd);
  if (instructions_fd < 0) {
    return errno == ENOENT
               ? CAI_OK
               : cai_set_error(error, CAI_ERR_INVALID,
                               "failed to read repository instructions");
  }
  rc = cai_smith_read_instructions_fd(allocator, instructions_fd, out, error);
  return rc;
}

static int cai_smith_append_instructions(const cai_allocator *allocator,
                                         char **out, const char *part,
                                         int prepend, cai_error *error) {
  char *merged;
  size_t old_length;
  size_t part_length;

  if (part == NULL || part[0] == '\0') {
    return CAI_OK;
  }
  old_length = *out != NULL ? strlen(*out) : 0U;
  part_length = strlen(part);
  if (old_length > CAI_SMITH_MAX_REPOSITORY_INSTRUCTIONS - part_length ||
      (old_length > 0U &&
       old_length > CAI_SMITH_MAX_REPOSITORY_INSTRUCTIONS - part_length - 2U)) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "combined agent instructions exceed Smith limit");
  }
  merged = (char *)cai_alloc(allocator, old_length + part_length +
                                            (old_length > 0U ? 2U : 0U) + 1U);
  if (merged == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate repository instructions");
  }
  if (prepend && old_length > 0U) {
    memcpy(merged, part, part_length);
    memcpy(merged + part_length, "\n\n", 2U);
    memcpy(merged + part_length + 2U, *out, old_length + 1U);
  } else if (old_length > 0U) {
    memcpy(merged, *out, old_length);
    memcpy(merged + old_length, "\n\n", 2U);
    memcpy(merged + old_length + 2U, part, part_length + 1U);
  } else {
    memcpy(merged, part, part_length + 1U);
  }
  cai_free_mem(allocator, *out);
  *out = merged;
  return CAI_OK;
}

static int cai_smith_default_config_directory(const cai_allocator *allocator,
                                              char **out, cai_error *error) {
  const char *base;
  const char *suffix;
  size_t base_length;
  size_t suffix_length;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent config directory output is required");
  }
  *out = NULL;
  base = getenv("XDG_CONFIG_HOME");
  suffix = "/cai";
  if (base == NULL || base[0] != '/') {
    base = getenv("HOME");
    suffix = "/.config/cai";
  }
  if (base == NULL || base[0] != '/') {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "XDG_CONFIG_HOME or HOME is required for agent policy");
  }
  base_length = strlen(base);
  suffix_length = strlen(suffix);
  if (base_length > SIZE_MAX - suffix_length - 1U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent config directory is too long");
  }
  *out = (char *)cai_alloc(allocator, base_length + suffix_length + 1U);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent config directory");
  }
  memcpy(*out, base, base_length);
  memcpy(*out + base_length, suffix, suffix_length + 1U);
  return CAI_OK;
}

static int
cai_smith_load_global_instruction_file(const cai_allocator *allocator,
                                       const char *path, char **out,
                                       cai_error *error) {
  int fd;
  int rc;

  *out = NULL;
  fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return errno == ENOENT ? CAI_OK
                           : cai_set_error(error, CAI_ERR_INVALID,
                                           "failed to read global AGENTS.md");
  }
  rc = cai_smith_read_instructions_fd(allocator, fd, out, error);
  return rc;
}

static int
cai_smith_load_global_instruction_store(const cai_allocator *allocator,
                                        const cai_blob_store *store, char **out,
                                        cai_error *error) {
  cai_source *source;
  cai_error source_error;
  char chunk[4096];
  char *part;
  size_t length;
  size_t nread;
  int rc;

  *out = NULL;
  source = NULL;
  rc = store->load(store->context, "AGENTS.md", &source, error);
  if (rc != CAI_OK || source == NULL) {
    return rc;
  }
  length = 0U;
  cai_error_init(&source_error);
  for (;;) {
    nread = source->read(source, chunk, sizeof(chunk), &source_error);
    if (nread == 0U) {
      break;
    }
    if (nread > CAI_SMITH_MAX_REPOSITORY_INSTRUCTIONS - length) {
      cai_source_close(source);
      cai_error_cleanup(&source_error);
      cai_free_mem(allocator, *out);
      *out = NULL;
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "global AGENTS.md exceeds Smith limit");
    }
    part = (char *)cai_realloc_mem(allocator, *out, length + nread + 1U);
    if (part == NULL) {
      cai_source_close(source);
      cai_error_cleanup(&source_error);
      cai_free_mem(allocator, *out);
      *out = NULL;
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to read global AGENTS.md");
    }
    *out = part;
    memcpy(*out + length, chunk, nread);
    length += nread;
    (*out)[length] = '\0';
  }
  cai_source_close(source);
  if (source_error.code != CAI_OK) {
    rc = cai_set_error_detail(error, source_error.code,
                              "failed to read global AGENTS.md from storage",
                              source_error.message);
    cai_error_cleanup(&source_error);
    cai_free_mem(allocator, *out);
    *out = NULL;
    return rc;
  }
  cai_error_cleanup(&source_error);
  return CAI_OK;
}

static int
cai_smith_load_repository_instructions(const cai_allocator *allocator,
                                       const cai_agent_preset_config *config,
                                       char **out, cai_error *error) {
  char resolved[PATH_MAX];
  char workspace_path[PATH_MAX];
  char canonical_workspace[PATH_MAX];
  char *global_path;
  char *global_part;
  char *part;
  char *cursor;
  const char *configured_directory;
  struct stat marker;
  int dir_fd;
  int rc;

  *out = NULL;
  if (config->global_instruction_store != NULL &&
      ((config->global_agents_md_path != NULL &&
        config->global_agents_md_path[0] != '\0') ||
       (config->agent_config_directory != NULL &&
        config->agent_config_directory[0] != '\0'))) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "global AGENTS.md storage and file settings are exclusive");
  }
  if (config->global_instruction_store != NULL &&
      config->global_instruction_store->load == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "global AGENTS.md storage requires a load callback");
  }
  global_path = NULL;
  global_part = NULL;
  part = NULL;
  if (config->global_instruction_store != NULL) {
    rc = cai_smith_load_global_instruction_store(
        allocator, config->global_instruction_store, &part, error);
  } else {
    if (config->global_agents_md_path != NULL &&
        config->global_agents_md_path[0] != '\0') {
      global_path = cai_strdup(allocator, config->global_agents_md_path);
      rc = global_path != NULL
               ? CAI_OK
               : cai_set_error(error, CAI_ERR_NOMEM,
                               "failed to allocate global AGENTS.md path");
    } else {
      configured_directory = config->agent_config_directory;
      if (configured_directory == NULL || configured_directory[0] == '\0') {
        rc = cai_smith_default_config_directory(allocator, &global_path, error);
      } else {
        size_t length = strlen(configured_directory);
        global_path =
            (char *)cai_alloc(allocator, length + sizeof("/AGENTS.md"));
        if (global_path == NULL) {
          rc = cai_set_error(error, CAI_ERR_NOMEM,
                             "failed to allocate global AGENTS.md path");
        } else {
          memcpy(global_path, configured_directory, length);
          memcpy(global_path + length, "/AGENTS.md", sizeof("/AGENTS.md"));
          rc = CAI_OK;
        }
      }
      if (rc == CAI_OK &&
          (config->global_agents_md_path == NULL ||
           config->global_agents_md_path[0] == '\0') &&
          (configured_directory == NULL || configured_directory[0] == '\0')) {
        size_t length = strlen(global_path);
        char *with_name = (char *)cai_realloc_mem(
            allocator, global_path, length + sizeof("/AGENTS.md"));
        if (with_name == NULL) {
          cai_free_mem(allocator, global_path);
          global_path = NULL;
          rc = cai_set_error(error, CAI_ERR_NOMEM,
                             "failed to allocate global AGENTS.md path");
        } else {
          global_path = with_name;
          memcpy(global_path + length, "/AGENTS.md", sizeof("/AGENTS.md"));
        }
      }
    }
    if (rc == CAI_OK) {
      rc = cai_smith_load_global_instruction_file(allocator, global_path, &part,
                                                  error);
    }
  }
  cai_free_mem(allocator, global_path);
  global_part = part;
  part = NULL;
  if (rc == CAI_OK && !config->codex_compat_agents_md) {
    rc = cai_smith_append_instructions(allocator, out, global_part, 0, error);
  }
  if (rc != CAI_OK || !config->codex_compat_agents_md) {
    cai_free_mem(allocator, global_part);
    if (rc == CAI_OK) {
      rc = cai_smith_load_workspace_instruction_file(
          allocator, config->workspace_directory, &part, error);
      if (rc == CAI_OK) {
        rc = cai_smith_append_instructions(allocator, out, part, 0, error);
      }
      cai_free_mem(allocator, part);
    }
    return rc;
  }
  if (realpath(config->workspace_directory, canonical_workspace) == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith workspace directory must exist");
  }
  if (strlen(canonical_workspace) >= sizeof(resolved)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Smith workspace path is too long");
  }
  memcpy(resolved, canonical_workspace, strlen(canonical_workspace) + 1U);
  cursor = resolved;
  for (;;) {
    dir_fd = open(cursor, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "failed to inspect Smith workspace hierarchy");
    }
    rc = fstatat(dir_fd, ".git", &marker, AT_SYMLINK_NOFOLLOW) == 0 ? 1 : 0;
    (void)close(dir_fd);
    if (rc || strcmp(cursor, "/") == 0) {
      break;
    }
    part = strrchr(cursor, '/');
    if (part == cursor) {
      cursor[1] = '\0';
    } else {
      *part = '\0';
    }
  }
  if (!rc) {
    rc = cai_smith_append_instructions(allocator, out, global_part, 0, error);
    cai_free_mem(allocator, global_part);
    if (rc == CAI_OK) {
      rc = cai_smith_load_workspace_instruction_file(
          allocator, canonical_workspace, &part, error);
    }
    if (rc == CAI_OK) {
      rc = cai_smith_append_instructions(allocator, out, part, 0, error);
    }
    cai_free_mem(allocator, part);
    return rc;
  }
  memcpy(workspace_path, canonical_workspace, strlen(canonical_workspace) + 1U);
  cursor = workspace_path;
  for (;;) {
    char *workspace_part;

    workspace_part = NULL;
    rc = cai_smith_load_workspace_instruction_file(allocator, cursor, &part,
                                                   error);
    if (rc == CAI_OK) {
      workspace_part = part;
      part = NULL;
      rc = cai_smith_append_instructions(allocator, out, workspace_part, 1,
                                         error);
    }
    cai_free_mem(allocator, workspace_part);
    if (rc != CAI_OK || strcmp(cursor, resolved) == 0) {
      break;
    }
    part = strrchr(cursor, '/');
    if (part == cursor) {
      cursor[1] = '\0';
    } else {
      *part = '\0';
    }
  }
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_smith_append_instructions(allocator, out, global_part, 1, error);
  cai_free_mem(allocator, global_part);
  global_part = NULL;
  if (rc != CAI_OK) {
    return rc;
  }
  return CAI_OK;
}

int cai_client_new_preset_agent(cai_client *client,
                                const cai_agent_preset *preset,
                                const cai_agent_preset_config *config,
                                cai_agent **out, cai_error *error) {
  cai_client_impl *client_impl;
  cai_agent_config agent_config;
  cai_read_tool_config read_config;
  cai_patch_tool_config patch_config;
  cai_terminal_tool_config terminal_config;
  cai_view_image_tool_config view_image_config;
  char *instructions;
  char *repository_instructions;
  char *skill_catalog_prompt;
  cai_skill_catalog *skill_catalog;
  const char *model;
  unsigned long tool_capabilities;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent output pointer is required");
  }
  *out = NULL;
  if (client == NULL || preset == NULL || preset->name == NULL ||
      preset->name[0] == '\0' || preset->prompt_version == NULL ||
      preset->prompt_version[0] == '\0' || preset->default_identity == NULL ||
      preset->default_identity[0] == '\0' || preset->default_model == NULL ||
      preset->default_model[0] == '\0' || config == NULL ||
      config->workspace_directory == NULL ||
      config->workspace_directory[0] == '\0') {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "agent preset, client, and workspace directory are required");
  }
  client_impl = CAI_CLIENT_IMPL(client);
  if (client_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is closed");
  }
  instructions = NULL;
  repository_instructions = NULL;
  skill_catalog_prompt = NULL;
  skill_catalog = NULL;
  model = config->model != NULL ? config->model : preset->default_model;
  tool_capabilities = preset->tool_capabilities;
  if (config->disable_terminal) {
    tool_capabilities &= ~CAI_AGENT_PRESET_TOOL_TERMINAL;
  }
  if (!cai_model_supports(model, CAI_MODEL_CAP_IMAGE_INPUT)) {
    tool_capabilities &= ~CAI_AGENT_PRESET_TOOL_VIEW_IMAGE;
  }
  rc = cai_smith_load_repository_instructions(&client_impl->allocator, config,
                                              &repository_instructions, error);
  if (rc == CAI_OK &&
      (tool_capabilities & CAI_AGENT_PRESET_TOOL_SKILLS) != 0UL) {
    rc = cai_skills_prepare(config->skills, config->agent_config_directory,
                            &skill_catalog, &skill_catalog_prompt, error);
    if (rc == CAI_OK && !cai_skills_catalog_has_entries(skill_catalog)) {
      tool_capabilities &= ~CAI_AGENT_PRESET_TOOL_SKILLS;
    }
  }
  if (rc == CAI_OK) {
    rc = cai_smith_render_instructions(
        &client_impl->allocator, preset, config, tool_capabilities,
        repository_instructions, skill_catalog_prompt, &instructions, error);
  }
  cai_free_mem(&client_impl->allocator, repository_instructions);
  cai_free_mem(NULL, skill_catalog_prompt);
  if (rc != CAI_OK) {
    if (skill_catalog != NULL) {
      cai_skills_catalog_cleanup(skill_catalog);
    }
    return rc;
  }
  cai_agent_config_init(&agent_config);
  agent_config.model = model;
  agent_config.developer_instructions = instructions;
  agent_config.reasoning_effort = config->reasoning_effort != NULL
                                      ? config->reasoning_effort
                                      : preset->default_reasoning_effort;
  /* Smith exposes the provider's approved reasoning summaries. Hosts can
   * choose any supported mode; the preset otherwise follows the provider's
   * auto selection, matching Codex's configurable request behavior. */
  agent_config.reasoning_summary =
      config->reasoning_summary != NULL
          ? config->reasoning_summary
          : (preset->default_reasoning_summary != NULL
                 ? preset->default_reasoning_summary
                 : CAI_REASONING_SUMMARY_AUTO);
  agent_config.tool_choice = CAI_TOOL_CHOICE_AUTO;
  agent_config.disable_parallel_tool_calls = 1;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  agent_config.enable_local_history = 1;
  agent_config.disable_auto_compaction = 1;
  rc = cai_client_new_agent(client, &agent_config, out, error);
  cai_free_mem(&client_impl->allocator, instructions);
  if (rc != CAI_OK) {
    if (skill_catalog != NULL) {
      cai_skills_catalog_cleanup(skill_catalog);
    }
    return rc;
  }
  if (!config->disable_terminal &&
      (preset->tool_capabilities & CAI_AGENT_PRESET_TOOL_TERMINAL) != 0UL) {
    memset(&terminal_config, 0, sizeof(terminal_config));
    if (config->terminal_tool_config != NULL) {
      terminal_config = *config->terminal_tool_config;
    }
    if (terminal_config.root_path != NULL &&
        strcmp(terminal_config.root_path, config->workspace_directory) != 0) {
      cai_agent_destroy(*out);
      *out = NULL;
      return cai_set_error(
          error, CAI_ERR_INVALID,
          "Smith terminal root must equal workspace directory");
    }
    terminal_config.root_path = config->workspace_directory;
    if (terminal_config.default_workdir == NULL) {
      terminal_config.default_workdir = config->workspace_directory;
    }
    rc = cai_agent_register_terminal_tools(*out, &terminal_config, error);
    if (rc != CAI_OK) {
      cai_agent_destroy(*out);
      *out = NULL;
      return rc;
    }
  }
  memset(&read_config, 0, sizeof(read_config));
  read_config.root_path = config->workspace_directory;
  read_config.default_workdir = config->workspace_directory;
  read_config.content_memory_limit = config->file_content_memory_limit;
  read_config.content_max_bytes = config->file_content_max_bytes;
  read_config.content_spool_dir = config->file_content_spool_dir;
  rc = (preset->tool_capabilities & CAI_AGENT_PRESET_TOOL_READ_FILE) != 0UL
           ? cai_agent_register_read_tool(*out, &read_config, error)
           : CAI_OK;
  if (rc == CAI_OK &&
      (preset->tool_capabilities & CAI_AGENT_PRESET_TOOL_LIST_FILES) != 0UL) {
    rc = cai_agent_register_list_files_tool(*out, &read_config, error);
  }
  if (rc == CAI_OK &&
      (preset->tool_capabilities & CAI_AGENT_PRESET_TOOL_APPLY_PATCH) != 0UL) {
    memset(&patch_config, 0, sizeof(patch_config));
    patch_config.root_path = config->workspace_directory;
    rc = cai_agent_register_patch_tool(*out, &patch_config, error);
  }
  if (rc == CAI_OK &&
      (preset->tool_capabilities & CAI_AGENT_PRESET_TOOL_VIEW_IMAGE) != 0UL &&
      cai_model_supports(agent_config.model, CAI_MODEL_CAP_IMAGE_INPUT)) {
    memset(&view_image_config, 0, sizeof(view_image_config));
    view_image_config.root_path = config->workspace_directory;
    view_image_config.default_workdir = config->workspace_directory;
    rc = cai_agent_register_view_image_tool(*out, &view_image_config, error);
  }
  if (rc == CAI_OK &&
      (tool_capabilities & CAI_AGENT_PRESET_TOOL_SKILLS) != 0UL &&
      cai_skills_catalog_has_entries(skill_catalog)) {
    rc = cai_agent_register_skill_tool_owned(*out, skill_catalog, error);
    if (rc == CAI_OK) {
      skill_catalog = NULL;
    }
  }
  if (rc != CAI_OK) {
    cai_agent_destroy(*out);
    *out = NULL;
  }
  if (skill_catalog != NULL) {
    cai_skills_catalog_cleanup(skill_catalog);
  }
  return rc;
}

int cai_client_new_smith_agent(cai_client *client,
                               const cai_smith_config *config, cai_agent **out,
                               cai_error *error) {
  cai_agent_preset preset;

  cai_agent_preset_from_smith(&preset);
  return cai_client_new_preset_agent(client, &preset, config, out, error);
}

int cai_client_new_preset_review_agent(cai_client *client,
                                       const cai_agent_preset *preset,
                                       const cai_agent_preset_config *config,
                                       cai_agent **out, cai_error *error) {
  cai_client_impl *client_impl;
  cai_agent_config agent_config;
  cai_read_tool_config read_config;
  cai_terminal_tool_config terminal_config;
  cai_view_image_tool_config view_image_config;
  const char *identity;
  const char *extension;
  char *repository_instructions;
  char *skill_catalog_prompt;
  cai_skill_catalog *skill_catalog;
  static const char *const review_suffix_parts[] = {
      ", a code reviewer running in CAI agent mode. You are acting as a "
      "reviewer for a proposed code change made by another engineer.\n\n",
      "Specific guidance from later developer, user, or applicable repository "
      "instructions overrides these general review guidelines.\n\n",
      "Flag a bug only when it meaningfully impacts accuracy, performance, "
      "security, or maintainability; is discrete and actionable; fits the "
      "repository's existing rigor; was introduced by the reviewed change; "
      "would likely be fixed by the author; does not rely on unstated "
      "assumptions; is proven to affect another part of the code rather than ",
      "merely suspected; and is clearly not an intentional change.\n\n",
      "Every finding body must explain why it is a bug, state the conditions "
      "needed for it to arise, communicate proportionate severity, stay within "
      "one concise paragraph, avoid code excerpts longer than three lines, and "
      "use a matter-of-fact, useful tone. Do not add flattery or ",
      "non-actionable comments.\n\n",
      "Return every qualifying finding that the author would fix if aware of "
      "it; prefer no findings over speculative ones. Ignore trivial style "
      "unless it obscures meaning or violates documented standards. Use one "
      "finding per distinct issue and deduplicate by changed location and "
      "defect/remedy.\n\n",
      "Use suggestion blocks only for concrete minimal replacement code; keep "
      "their exact leading whitespace and do not change outer indentation "
      "unless that is the fix.\n\n",
      "Inspect applicable root and scoped project instructions for changed "
      "files. More-specific guidance wins. For a rule-supported finding, ",
      "verify ",
      "the smallest supporting instruction-file line range and include one "
      "compact local-file or Markdown reference in the body. Do not invent "
      "findings solely because an instruction file exists.\n\n",
      "Choose the shortest code location that makes the issue clear, normally "
      "no more than 5-10 lines, and ensure it overlaps the reviewed diff. Use "
      "[P0] only for universal release blockers, [P1] for next-cycle urgent "
      "issues, [P2] for normal issues, and [P3] for low-priority issues.\n\n",
      "Your final response MUST be exactly one JSON object with this shape: "
      "{\"findings\":[{\"title\":\"[P1] imperative title under 80 chars\","
      "\"body\":\"one-paragraph impact and cause\","
      "\"confidence_score\":0.0,\"priority\":1,"
      "\"code_location\":{\"absolute_file_path\":\"/absolute/path\","
      "\"line_range\":{\"start\":1,\"end\":1}}}],",
      "\"overall_correctness\":\"patch is correct\"|"
      "\"patch is incorrect\",\"overall_explanation\":\"brief reason\","
      "\"overall_confidence_score\":0.0}. The code_location field is ",
      "required for every finding and must overlap the reviewed diff. Set "
      "priority to 0-3 or omit it when undetermined. Correct means existing "
      "code and tests will not break and the patch is free of bugs or other "
      "blocking issues; ignore non-blocking style, formatting, typo, and ",
      "documentation nits. Return an empty findings array when there are no "
      "qualifying findings. Do not wrap the JSON in Markdown or include extra ",
      "prose. Do not generate a PR fix.\n"};
  char *instructions;
  size_t length;
  size_t offset;
  size_t repository_length;
  size_t skill_catalog_length;
  size_t extension_length;
  size_t i;
  int rc;

  if ((preset != NULL) &&
      (preset->review_tool_capabilities &
       ~(CAI_AGENT_PRESET_TOOL_READ_FILE | CAI_AGENT_PRESET_TOOL_LIST_FILES |
         CAI_AGENT_PRESET_TOOL_TERMINAL | CAI_AGENT_PRESET_TOOL_VIEW_IMAGE |
         CAI_AGENT_PRESET_TOOL_SKILLS)) != 0UL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "review presets support only read, list, terminal, "
                         "view-image, and skills capabilities");
  }

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "review agent output pointer is required");
  }
  *out = NULL;
  if (client == NULL || preset == NULL || !preset->supports_review ||
      preset->name == NULL || preset->name[0] == '\0' ||
      preset->prompt_version == NULL || preset->prompt_version[0] == '\0' ||
      preset->default_identity == NULL || preset->default_identity[0] == '\0' ||
      preset->default_model == NULL || preset->default_model[0] == '\0' ||
      config == NULL || config->workspace_directory == NULL ||
      config->workspace_directory[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "review-capable agent preset, client, and workspace "
                         "directory are required");
  }
  client_impl = CAI_CLIENT_IMPL(client);
  if (client_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is closed");
  }
  instructions = NULL;
  repository_instructions = NULL;
  skill_catalog_prompt = NULL;
  skill_catalog = NULL;
  identity = config->agent_identity != NULL ? config->agent_identity
                                            : preset->default_identity;
  extension = config->developer_instructions_extension;
  if (identity[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "review agent identity is invalid");
  }
  rc = cai_smith_load_repository_instructions(&client_impl->allocator, config,
                                              &repository_instructions, error);
  if (rc == CAI_OK && (preset->review_tool_capabilities &
                       CAI_AGENT_PRESET_TOOL_SKILLS) != 0UL) {
    rc = cai_skills_prepare(config->skills, config->agent_config_directory,
                            &skill_catalog, &skill_catalog_prompt, error);
  }
  if (rc != CAI_OK) {
    return rc;
  }
  if (preset->review_developer_instructions != NULL) {
    rc = cai_preset_render_template(
        &client_impl->allocator, preset->review_developer_instructions, preset,
        config, repository_instructions, skill_catalog_prompt, NULL,
        &instructions, error);
    if (rc != CAI_OK) {
      goto review_instructions_failed;
    }
    goto review_instructions_ready;
  }
  if (strlen(identity) > SIZE_MAX - strlen("You are ") - 1U) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "Smith review instructions are too large");
    goto review_instructions_failed;
  }
  length = strlen("You are ") + strlen(identity);
  for (i = 0U; i < sizeof(review_suffix_parts) / sizeof(review_suffix_parts[0]);
       i++) {
    if (strlen(review_suffix_parts[i]) > SIZE_MAX - length) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "Smith review instructions are too large");
      goto review_instructions_failed;
    }
    length += strlen(review_suffix_parts[i]);
  }
  repository_length =
      repository_instructions != NULL ? strlen(repository_instructions) : 0U;
  if (repository_length > 0U) {
    if (length > SIZE_MAX - 2U - repository_length - 1U) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "Smith review instructions are too large");
      goto review_instructions_failed;
    }
    length += 2U + repository_length;
  }
  skill_catalog_length =
      skill_catalog_prompt != NULL ? strlen(skill_catalog_prompt) : 0U;
  if (skill_catalog_length > 0U) {
    if (length > SIZE_MAX - 2U - skill_catalog_length - 1U) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "Smith review instructions are too large");
      goto review_instructions_failed;
    }
    length += 2U + skill_catalog_length;
  }
  extension_length = extension != NULL ? strlen(extension) : 0U;
  if (extension_length > 0U &&
      (length > SIZE_MAX - 2U ||
       extension_length > SIZE_MAX - length - 2U - 1U)) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "Smith review instructions are too large");
    goto review_instructions_failed;
  }
  if (extension_length > 0U) {
    length += 2U + extension_length;
  }
  instructions = (char *)cai_alloc(&client_impl->allocator, length + 1U);
  if (instructions == NULL) {
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "failed to allocate Smith review instructions");
    goto review_instructions_failed;
  }
  memcpy(instructions, "You are ", strlen("You are "));
  offset = strlen("You are ");
  memcpy(instructions + offset, identity, strlen(identity));
  offset += strlen(identity);
  for (i = 0U; i < sizeof(review_suffix_parts) / sizeof(review_suffix_parts[0]);
       i++) {
    size_t part_length;

    part_length = strlen(review_suffix_parts[i]);
    memcpy(instructions + offset, review_suffix_parts[i], part_length);
    offset += part_length;
  }
  if (repository_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    offset += 2U;
    memcpy(instructions + offset, repository_instructions, repository_length);
    offset += repository_length;
  }
  if (skill_catalog_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    offset += 2U;
    memcpy(instructions + offset, skill_catalog_prompt, skill_catalog_length);
    offset += skill_catalog_length;
  }
  if (extension_length > 0U) {
    memcpy(instructions + offset, "\n\n", 2U);
    memcpy(instructions + offset + 2U, extension, extension_length);
    offset += 2U + extension_length;
  }
  instructions[length] = '\0';
review_instructions_ready:
  cai_free_mem(&client_impl->allocator, repository_instructions);
  repository_instructions = NULL;
  cai_free_mem(NULL, skill_catalog_prompt);
  skill_catalog_prompt = NULL;
  cai_agent_config_init(&agent_config);
  agent_config.model =
      config->model != NULL ? config->model : preset->default_model;
  agent_config.developer_instructions = instructions;
  agent_config.reasoning_effort = config->reasoning_effort != NULL
                                      ? config->reasoning_effort
                                      : preset->default_reasoning_effort;
  agent_config.reasoning_summary =
      config->reasoning_summary != NULL
          ? config->reasoning_summary
          : (preset->default_reasoning_summary != NULL
                 ? preset->default_reasoning_summary
                 : CAI_REASONING_SUMMARY_AUTO);
  agent_config.tool_choice = CAI_TOOL_CHOICE_AUTO;
  agent_config.disable_parallel_tool_calls = 1;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  agent_config.enable_local_history = 1;
  agent_config.disable_auto_compaction = 1;
  rc = cai_client_new_agent(client, &agent_config, out, error);
  cai_free_mem(&client_impl->allocator, instructions);
  if (rc != CAI_OK) {
    if (skill_catalog != NULL) {
      cai_skills_catalog_cleanup(skill_catalog);
    }
    return rc;
  }
  memset(&read_config, 0, sizeof(read_config));
  read_config.root_path = config->workspace_directory;
  read_config.default_workdir = config->workspace_directory;
  read_config.content_memory_limit = config->file_content_memory_limit;
  read_config.content_max_bytes = config->file_content_max_bytes;
  read_config.content_spool_dir = config->file_content_spool_dir;
  rc = (preset->review_tool_capabilities & CAI_AGENT_PRESET_TOOL_READ_FILE) !=
               0UL
           ? cai_agent_register_read_tool(*out, &read_config, error)
           : CAI_OK;
  if (rc == CAI_OK && (preset->review_tool_capabilities &
                       CAI_AGENT_PRESET_TOOL_LIST_FILES) != 0UL) {
    rc = cai_agent_register_list_files_tool(*out, &read_config, error);
  }
  if (rc == CAI_OK && !config->disable_terminal &&
      (preset->review_tool_capabilities & CAI_AGENT_PRESET_TOOL_TERMINAL) !=
          0UL) {
    memset(&terminal_config, 0, sizeof(terminal_config));
    if (config->terminal_tool_config != NULL) {
      terminal_config = *config->terminal_tool_config;
    }
    if (terminal_config.root_path != NULL &&
        strcmp(terminal_config.root_path, config->workspace_directory) != 0) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "Smith terminal root must equal workspace directory");
    }
    terminal_config.root_path = config->workspace_directory;
    if (rc == CAI_OK && terminal_config.default_workdir == NULL) {
      terminal_config.default_workdir = config->workspace_directory;
    }
    if (rc == CAI_OK) {
      rc = cai_agent_register_terminal_tools(*out, &terminal_config, error);
    }
  }
  if (rc == CAI_OK &&
      (preset->review_tool_capabilities & CAI_AGENT_PRESET_TOOL_VIEW_IMAGE) !=
          0UL &&
      cai_model_supports(agent_config.model, CAI_MODEL_CAP_IMAGE_INPUT)) {
    memset(&view_image_config, 0, sizeof(view_image_config));
    view_image_config.root_path = config->workspace_directory;
    view_image_config.default_workdir = config->workspace_directory;
    rc = cai_agent_register_view_image_tool(*out, &view_image_config, error);
  }
  if (rc == CAI_OK &&
      (preset->review_tool_capabilities & CAI_AGENT_PRESET_TOOL_SKILLS) !=
          0UL &&
      cai_skills_catalog_has_entries(skill_catalog)) {
    rc = cai_agent_register_skill_tool_owned(*out, skill_catalog, error);
    if (rc == CAI_OK) {
      skill_catalog = NULL;
    }
  }
  if (rc != CAI_OK) {
    cai_agent_destroy(*out);
    *out = NULL;
  }
  if (skill_catalog != NULL) {
    cai_skills_catalog_cleanup(skill_catalog);
  }
  return rc;

review_instructions_failed:
  cai_free_mem(&client_impl->allocator, repository_instructions);
  cai_free_mem(NULL, skill_catalog_prompt);
  if (skill_catalog != NULL) {
    cai_skills_catalog_cleanup(skill_catalog);
  }
  return rc;
}

int cai_client_new_smith_review_agent(cai_client *client,
                                      const cai_smith_config *config,
                                      cai_agent **out, cai_error *error) {
  cai_agent_preset preset;

  cai_agent_preset_from_smith(&preset);
  return cai_client_new_preset_review_agent(client, &preset, config, out,
                                            error);
}
