#include <cai/cai.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CAI_MIKE_MIND_DEFAULT_SKILL_DIR
#define CAI_MIKE_MIND_DEFAULT_SKILL_DIR "../parallax/skills/mike-mind"
#endif

typedef struct prompt_buffer {
  char *data;
  size_t length;
  size_t capacity;
} prompt_buffer;

static const char *mike_mind_files[] = {
    "SKILL.md",
    "references/source-index.md",
    "references/mike-worldview.md",
    "references/mike-expertise.md",
    "references/mike-broader-patterns.md",
    "references/mike-doctrine-deep.md",
    "references/mike-framework-judgments.md",
    "references/mike-case-reasoning.md",
    "references/mike-judgment-playbooks.md",
    "references/mike-question-routing.md",
    "references/mike-anti-pattern-atlas.md",
    "references/mike-vocabulary-and-recurring-lines.md",
    "references/mike-factual-memory.md",
    "references/mike-negative-space-inference.md",
    "references/mike-voice-and-inference.md",
    "references/source-syntheses/business-idea-and-market-evaluation.md",
    "references/source-syntheses/career-profile-authority.md",
    "references/source-syntheses/centaur-manifest.md",
    "references/source-syntheses/code-review-and-verification.md",
    "references/source-syntheses/dashboard-and-local-optimization.md",
    "references/source-syntheses/framework-scorecards.md",
    "references/source-syntheses/intent-as-infrastructure.md",
    "references/source-syntheses/millennium-and-procurement.md",
    "references/source-syntheses/mission-command-and-obaf.md",
    "references/source-syntheses/obaf-deep.md",
    "references/source-syntheses/organizational-models.md",
    "references/source-syntheses/steering-and-craft.md"};

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static int prompt_reserve(prompt_buffer *buffer, size_t extra) {
  char *grown;
  size_t needed;
  size_t capacity;

  needed = buffer->length + extra + 1U;
  if (needed <= buffer->capacity) {
    return 1;
  }
  capacity = buffer->capacity == 0U ? 8192U : buffer->capacity;
  while (capacity < needed) {
    capacity *= 2U;
  }
  grown = (char *)realloc(buffer->data, capacity);
  if (grown == NULL) {
    return 0;
  }
  buffer->data = grown;
  buffer->capacity = capacity;
  return 1;
}

static int prompt_append(prompt_buffer *buffer, const char *text) {
  size_t len;

  len = strlen(text);
  if (!prompt_reserve(buffer, len)) {
    return 0;
  }
  memcpy(buffer->data + buffer->length, text, len);
  buffer->length += len;
  buffer->data[buffer->length] = '\0';
  return 1;
}

static int prompt_append_file(prompt_buffer *buffer, const char *root,
                              const char *relative_path) {
  char path[4096];
  char chunk[4096];
  FILE *fp;
  size_t n;

  if (snprintf(path, sizeof(path), "%s/%s", root, relative_path) < 0 ||
      strlen(root) + strlen(relative_path) + 2U > sizeof(path)) {
    fprintf(stderr, "skill path is too long: %s/%s\n", root, relative_path);
    return 0;
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    perror(path);
    return 0;
  }
  if (!prompt_append(buffer, "\n\n--- BEGIN ") ||
      !prompt_append(buffer, relative_path) ||
      !prompt_append(buffer, " ---\n")) {
    fclose(fp);
    return 0;
  }
  for (;;) {
    n = fread(chunk, 1U, sizeof(chunk), fp);
    if (n > 0U) {
      if (!prompt_reserve(buffer, n)) {
        fclose(fp);
        return 0;
      }
      memcpy(buffer->data + buffer->length, chunk, n);
      buffer->length += n;
      buffer->data[buffer->length] = '\0';
    }
    if (n < sizeof(chunk)) {
      if (ferror(fp)) {
        perror(path);
        fclose(fp);
        return 0;
      }
      break;
    }
  }
  fclose(fp);
  return prompt_append(buffer, "\n--- END ") &&
         prompt_append(buffer, relative_path) &&
         prompt_append(buffer, " ---\n");
}

static int build_mike_mind_prompt(prompt_buffer *buffer,
                                  const char *skill_dir) {
  size_t i;

  buffer->data = NULL;
  buffer->length = 0U;
  buffer->capacity = 0U;
  if (!prompt_append(buffer,
                     "You are a cai example agent implementing the Mike Mind "
                     "skill. Use the embedded skill and reference material as "
                     "your full system prompt. Answer as the skill instructs; "
                     "do not mention repository paths or source files unless "
                     "the user explicitly asks about implementation.\n")) {
    return 0;
  }
  for (i = 0U; i < sizeof(mike_mind_files) / sizeof(mike_mind_files[0]); i++) {
    if (!prompt_append_file(buffer, skill_dir, mike_mind_files[i])) {
      return 0;
    }
  }
  return 1;
}

static int stdout_sink_write(void *context, const void *bytes, size_t count,
                             cai_error *error) {
  (void)context;
  (void)error;
  if (count == 0U) {
    return CAI_OK;
  }
  if (fwrite(bytes, 1U, count, stdout) != count) {
    return CAI_ERR_TRANSPORT;
  }
  fflush(stdout);
  return CAI_OK;
}

static void trim_newline(char *line) {
  size_t length;

  length = strlen(line);
  while (length > 0U &&
         (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
    line[length - 1U] = '\0';
    length--;
  }
}

int main(void) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_sink_callbacks sink_callbacks;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_sink *sink;
  cai_error error;
  prompt_buffer prompt;
  const char *skill_dir;
  char line[4096];
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  memset(&prompt, 0, sizeof(prompt));
  skill_dir = getenv("CAI_MIKE_MIND_SKILL_DIR");
  if (skill_dir == NULL || skill_dir[0] == '\0') {
    skill_dir = CAI_MIKE_MIND_DEFAULT_SKILL_DIR;
  }
  client = NULL;
  agent = NULL;
  session = NULL;
  sink = NULL;
  exit_code = 1;

  if (!build_mike_mind_prompt(&prompt, skill_dir)) {
    fprintf(stderr, "failed to build Mike Mind prompt from %s\n", skill_dir);
    goto done;
  }
  agent_config.model = CAI_MODEL_GPT_5_NANO;
  agent_config.instructions = prompt.data;
  agent_config.auto_compact = 1;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open", rc, &error);
    goto done;
  }
  rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_new_agent", rc, &error);
    goto done;
  }
  rc = cai_agent_new_session(agent, &session, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_new_session", rc, &error);
    goto done;
  }
  sink_callbacks.write = stdout_sink_write;
  sink_callbacks.close = NULL;
  sink_callbacks.context = NULL;
  rc = cai_sink_from_callbacks(&sink_callbacks, &sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_sink_from_callbacks", rc, &error);
    goto done;
  }

  for (;;) {
    fputs("mike> ", stdout);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL) {
      exit_code = 0;
      break;
    }
    trim_newline(line);
    if (line[0] == '\0') {
      continue;
    }
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
      exit_code = 0;
      break;
    }
    rc = cai_session_add_text(session, "user", line, &error);
    if (rc == CAI_OK) {
      rc = cai_session_stream_text(session, sink, &error);
    }
    fputc('\n', stdout);
    if (rc != CAI_OK) {
      exit_code = print_error("cai_session_stream_text", rc, &error);
      break;
    }
  }

done:
  cai_sink_close(sink);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  free(prompt.data);
  return exit_code;
}
