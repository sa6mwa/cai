#include "cai_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAI_ENV_LINE_LIMIT 8192U

static const char *cai_skip_space(const char *text) {
  while (*text != '\0' && isspace((unsigned char)*text)) {
    text++;
  }
  return text;
}

static size_t cai_trimmed_length(const char *text, size_t length) {
  while (length > 0U && isspace((unsigned char)text[length - 1U])) {
    length--;
  }
  return length;
}

static int cai_key_matches(const char *line, const char **value_start) {
  static const char key[] = "OPENAI_API_KEY";
  const char *cursor;
  size_t key_len;

  cursor = cai_skip_space(line);
  if (*cursor == '#') {
    return 0;
  }
  if (strncmp(cursor, "export", 6U) == 0 && isspace((unsigned char)cursor[6])) {
    cursor = cai_skip_space(cursor + 6);
  }
  key_len = sizeof(key) - 1U;
  if (strncmp(cursor, key, key_len) != 0) {
    return 0;
  }
  cursor += key_len;
  cursor = cai_skip_space(cursor);
  if (*cursor != '=') {
    return 0;
  }
  cursor++;
  cursor = cai_skip_space(cursor);
  *value_start = cursor;
  return 1;
}

static int cai_parse_env_value(const cai_allocator *allocator,
                               const char *value_start, char **out,
                               cai_error *error) {
  const char *cursor;
  char quote;
  size_t length;

  cursor = value_start;
  quote = '\0';
  if (*cursor == '\'' || *cursor == '"') {
    quote = *cursor;
    cursor++;
    value_start = cursor;
    while (*cursor != '\0' && *cursor != quote) {
      cursor++;
    }
    length = (size_t)(cursor - value_start);
  } else {
    while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') {
      cursor++;
    }
    length = cai_trimmed_length(value_start, (size_t)(cursor - value_start));
  }
  if (length == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "OPENAI_API_KEY in .env is empty");
  }
  *out = cai_strndup(allocator, value_start, length);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate OPENAI_API_KEY");
  }
  return CAI_OK;
}

static int cai_load_dotenv_api_key(const cai_allocator *allocator, char **out,
                                   cai_error *error) {
  FILE *fp;
  char line[CAI_ENV_LINE_LIMIT];
  const char *value_start;

  fp = fopen(".env", "r");
  if (fp == NULL) {
    return CAI_ERR_CANCELLED;
  }
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (cai_key_matches(line, &value_start)) {
      fclose(fp);
      return cai_parse_env_value(allocator, value_start, out, error);
    }
  }
  fclose(fp);
  return cai_set_error(error, CAI_ERR_INVALID,
                       ".env exists but OPENAI_API_KEY is not set");
}

int cai_resolve_api_key(const cai_allocator *allocator,
                        const char *explicit_key, char **out,
                        cai_error *error) {
  const char *env_key;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "api key output pointer is required");
  }
  *out = NULL;
  if (explicit_key != NULL && explicit_key[0] != '\0') {
    *out = cai_strdup(allocator, explicit_key);
    if (*out == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate explicit API key");
    }
    return CAI_OK;
  }
  rc = cai_load_dotenv_api_key(allocator, out, error);
  if (rc != CAI_ERR_CANCELLED) {
    return rc;
  }
  env_key = getenv("OPENAI_API_KEY");
  if (env_key == NULL || env_key[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "OPENAI_API_KEY is not configured");
  }
  *out = cai_strdup(allocator, env_key);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate OPENAI_API_KEY");
  }
  return CAI_OK;
}
