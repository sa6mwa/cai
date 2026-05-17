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

static int cai_env_name_valid(const char *key) {
  const char *cursor;

  if (key == NULL || key[0] == '\0') {
    return 0;
  }
  cursor = key;
  if (!isalpha((unsigned char)*cursor) && *cursor != '_') {
    return 0;
  }
  cursor++;
  while (*cursor != '\0') {
    if (!isalnum((unsigned char)*cursor) && *cursor != '_') {
      return 0;
    }
    cursor++;
  }
  return 1;
}

static int cai_key_matches(const char *line, const char *key,
                           const char **value_start) {
  const char *cursor;
  size_t key_len;

  cursor = cai_skip_space(line);
  if (*cursor == '#') {
    return 0;
  }
  if (strncmp(cursor, "export", 6U) == 0 && isspace((unsigned char)cursor[6])) {
    cursor = cai_skip_space(cursor + 6);
  }
  key_len = strlen(key);
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

static int cai_parse_env_value(const cai_allocator *allocator, const char *key,
                               const char *value_start, char **out,
                               cai_error *error) {
  const char *cursor;
  char quote;
  size_t length;
  size_t i;

  cursor = value_start;
  quote = '\0';
  if (*cursor == '\'' || *cursor == '"') {
    quote = *cursor;
    cursor++;
    value_start = cursor;
    while (*cursor != '\0' && *cursor != quote) {
      cursor++;
    }
    if (*cursor != quote) {
      char message[160];

      snprintf(message, sizeof(message), "%s in dotenv file has no closing quote",
               key);
      return cai_set_error(error, CAI_ERR_INVALID, message);
    }
    length = (size_t)(cursor - value_start);
  } else {
    while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') {
      cursor++;
    }
    length = cai_trimmed_length(value_start, (size_t)(cursor - value_start));
  }
  if (length == 0U) {
    char message[128];

    snprintf(message, sizeof(message), "%s in dotenv file is empty", key);
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  for (i = 0U; i < length; i++) {
    unsigned char ch;

    ch = (unsigned char)value_start[i];
    if (ch < 0x20U || ch == 0x7fU) {
      char message[160];

      snprintf(message, sizeof(message),
               "%s in dotenv file contains a control character", key);
      return cai_set_error(error, CAI_ERR_INVALID, message);
    }
  }
  *out = cai_strndup(allocator, value_start, length);
  if (*out == NULL) {
    char message[128];

    snprintf(message, sizeof(message), "failed to allocate %s", key);
    return cai_set_error(error, CAI_ERR_NOMEM, message);
  }
  return CAI_OK;
}

static int cai_load_dotenv_api_key_internal(const cai_allocator *allocator,
                                            const char *dotenv_path,
                                            const char *env_name, char **out,
                                            cai_error *error) {
  FILE *fp;
  char line[CAI_ENV_LINE_LIMIT];
  const char *value_start;

  if (dotenv_path == NULL || dotenv_path[0] == '\0') {
    return CAI_ERR_CANCELLED;
  }
  fp = fopen(dotenv_path, "r");
  if (fp == NULL) {
    return CAI_ERR_CANCELLED;
  }
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strchr(line, '\n') == NULL && !feof(fp)) {
      fclose(fp);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "dotenv file contains an overlong line");
    }
    if (cai_key_matches(line, env_name, &value_start)) {
      fclose(fp);
      return cai_parse_env_value(allocator, env_name, value_start, out, error);
    }
  }
  fclose(fp);
  {
    char message[160];

    snprintf(message, sizeof(message), "dotenv file exists but %s is not set",
             env_name);
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
}

int cai_resolve_api_key(const cai_allocator *allocator,
                        const char *explicit_key, const char *env_name,
                        char **out, cai_error *error) {
  const char *env_key;
  const char *key_name;

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
  key_name = env_name != NULL && env_name[0] != '\0' ? env_name
                                                     : CAI_OPENAI_API_KEY_ENV;
  if (!cai_env_name_valid(key_name)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "api key environment variable name is invalid");
  }
  env_key = getenv(key_name);
  if (env_key == NULL || env_key[0] == '\0') {
    char message[128];

    snprintf(message, sizeof(message), "%s is not configured", key_name);
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  *out = cai_strdup(allocator, env_key);
  if (*out == NULL) {
    char message[128];

    snprintf(message, sizeof(message), "failed to allocate %s", key_name);
    return cai_set_error(error, CAI_ERR_NOMEM, message);
  }
  return CAI_OK;
}

int cai_load_dotenv_api_key(const char *dotenv_path, const char *api_key_env,
                            char **out, cai_error *error) {
  const char *key_name;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "api key output pointer is required");
  }
  *out = NULL;
  key_name = api_key_env != NULL && api_key_env[0] != '\0'
                 ? api_key_env
                 : CAI_OPENAI_API_KEY_ENV;
  if (!cai_env_name_valid(key_name)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "api key environment variable name is invalid");
  }
  if (dotenv_path == NULL || dotenv_path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "dotenv path is required");
  }
  return cai_load_dotenv_api_key_internal(NULL, dotenv_path, key_name, out,
                                          error);
}

void cai_string_destroy(char *value) { cai_free_mem(NULL, value); }
