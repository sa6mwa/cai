#include "runtime_options.h"

void cai_cli_apply_runtime_options(const cai_cli_options *options,
                                   cai_agent_runtime_config *config,
                                   cai_agent_preset *preset,
                                   cai_skill_config *skills) {
  cai_agent_preset_from_smith(preset);
  preset->default_model = options->model;
  preset->default_reasoning_effort = options->reasoning_effort;
  preset->default_reasoning_summary = options->reasoning_summary;
  config->preset_descriptor = preset;
  cai_skill_config_init(skills);
  if (options->review)
    config->preset = CAI_SMITH_REVIEW_PRESET;
  skills->skills_directory = options->skills_dir;
  config->disable_chrome = options->review || !options->chrome;
  config->chrome_config.socket_path = options->chrome_socket;
  config->disable_web_search = !options->web_search;
  config->disable_read_file = !options->read_file;
  config->disable_list_files = !options->list_files;
  config->disable_apply_patch = !options->apply_patch;
  config->disable_view_image = !options->view_image;
  config->disable_goals = !options->goals;
  config->disable_skills = !options->skills;
  config->enable_todo = !options->review && options->todo;
  if (options->board != NULL)
    config->todo.default_board = options->board;
  config->enable_searxng = !options->review && options->searxng;
  config->enable_revgeo = !options->review && options->revgeo;
  if (options->searxng_url != NULL)
    config->searxng.base_url = options->searxng_url;
  if (options->revgeo_url != NULL)
    config->revgeo.base_url = options->revgeo_url;
  config->model = options->model_explicit ? options->model : NULL;
  config->reasoning_effort =
      options->reasoning_effort_explicit ? options->reasoning_effort : NULL;
  config->reasoning_tool.enabled = !options->review && options->reasoning_tool;
  config->reasoning_tool.normal_effort = options->normal_reasoning_effort;
  config->reasoning_tool.high_effort = options->high_reasoning_effort;
  config->reasoning_summary =
      options->reasoning_summary_explicit ? options->reasoning_summary : NULL;
  config->review_model = options->review_model;
  config->service_tier = options->service_tier;
  config->review_reasoning_effort = options->review_reasoning_effort;
  config->review_reasoning_summary = options->review_reasoning_summary;
  if (options->config_dir != NULL)
    config->agent_config_directory = options->config_dir;
  if (options->agents_md != NULL)
    config->global_agents_md_path = options->agents_md;
  config->skills = options->skills_dir != NULL ? skills : NULL;
  config->agent_identity = options->identity;
  config->developer_instructions_extension = options->developer_instructions;
  config->codex_compat_agents_md = options->codex_agents_md;
  config->shell_context_max_lines = options->shell_context_max_lines;
  config->shell_context_max_bytes = options->shell_context_max_bytes;
  config->enable_image_generation =
      !options->review && options->image_generation;
  config->disable_terminal = !options->terminal;
  config->disable_review_subagent = !options->review_subagent;
  config->disable_subagent_steering = !options->subagent_steering;
}
