#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cai_internal.h"
#include <cai/config.h>
#include <cai/smith.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <xlocale.h>
#endif

#define CONFIG_LINE_MAX (1024U * 1024U)
#define CONFIG_ENTRY_MAX 8192U

typedef enum config_storage {
  S_STR,
  S_BOOL,
  S_NOTBOOL,
  S_INT,
  S_LONG,
  S_UINT,
  S_SIZE,
  S_I64,
  S_REAL,
  S_PROTOCOL,
  S_CONTINUITY
} config_storage;
typedef enum config_target {
  T_CLIENT,
  T_AGENT,
  T_RUNTIME,
  T_TERMINAL
} config_target;
typedef struct config_binding {
  const char *section, *key;
  config_storage storage;
  config_target target;
  size_t offset;
  unsigned int flags;
  const char *choices, *description;
} config_binding;
#define C_CLIENT cai_client_config
#define C_AGENT cai_agent_config
#define C_RUNTIME cai_agent_runtime_config
#define C_TERMINAL cai_terminal_tool_config
#define F(section, key, storage, target, member, flags, choices, description)  \
  {section, key,     S_##storage, T_##target, offsetof(C_##target, member),    \
   flags,   choices, description},
static const config_binding bindings[] = {
#include "cai_config_fields.inc"
};
#undef F
static const cai_config_field globals[] = {
    {"mcp.*", "enabled", CAI_CONFIG_BOOL, "true",
     "Enable this named HTTP MCP server. Copy the example section per server.",
     NULL, 0U},
    {"mcp.*", "endpoint", CAI_CONFIG_STRING, "",
     "Streamable HTTP MCP URL; required for enabled servers.", NULL, 0U},
    {"mcp.*", "api_key_env", CAI_CONFIG_STRING, "",
     "Optional environment variable containing a Bearer API key.", NULL, 0U},
    {"mcp.*", "discovery_timeout_ms", CAI_CONFIG_INTEGER, "3000",
     "Whole initialize/tools-list discovery budget per server in milliseconds.",
     NULL, 0U},
    {"mcp.*", "timeout_ms", CAI_CONFIG_INTEGER, "30000",
     "Normal MCP tool HTTP request timeout in milliseconds.", NULL, 0U},
    {"", "config_env_has_precedence", CAI_CONFIG_BOOL, "false",
     "Config [env] replaces existing process variables when true.", NULL, 0U},
    {"client", "provider", CAI_CONFIG_STRING, "openai",
     "Provider preset; host supplies auth objects.",
     "chatgpt|openai|openrouter|berget|custom", 0U},
    {"review", "allowed_models", CAI_CONFIG_STRINGS, NULL,
     "Permitted reviewer model overrides; repeat the key.", NULL, 0U},
    {"review", "allowed_reasoning_efforts", CAI_CONFIG_STRINGS, NULL,
     "Permitted reviewer effort overrides; repeat the key.", NULL, 0U},
    {"review", "allowed_reasoning_summaries", CAI_CONFIG_STRINGS, NULL,
     "Permitted reviewer summary overrides; repeat the key.", NULL, 0U}};

typedef struct config_entry {
  char *section, *key;
  char **values;
  size_t count;
  unsigned long line;
} config_entry;
typedef struct config_owned_field {
  cai_config_field field;
  char *strings[5];
} config_owned_field;
static void field_cleanup(config_owned_field *field) {
  size_t i;
  for (i = 0U; i < 5U; i++)
    cai_free_mem(NULL, field->strings[i]);
}
typedef struct config_impl {
  config_owned_field *fields;
  size_t field_count;
  config_entry *entries;
  size_t count;
  char *origin;
  locale_t numeric_locale;
} config_impl;
#define IMPL(c) ((config_impl *)(c)->impl)

static int config_error(cai_error *error, const char *origin,
                        unsigned long line, const char *section,
                        const char *key, const char *reason) {
  char message[1024];
  snprintf(message, sizeof(message), "%s:%lu: [%s] %s: %s",
           origin != NULL ? origin : "configuration", line,
           section != NULL ? section : "", key != NULL ? key : "", reason);
  return cai_set_error(error, CAI_ERR_INVALID, message);
}
static void entry_cleanup(config_entry *entry) {
  size_t i;
  for (i = 0U; i < entry->count; i++)
    cai_free_mem(NULL, entry->values[i]);
  cai_free_mem(NULL, entry->values);
  cai_free_mem(NULL, entry->section);
  cai_free_mem(NULL, entry->key);
}
static void entries_cleanup(config_impl *impl) {
  size_t i;
  for (i = 0U; i < impl->count; i++)
    entry_cleanup(&impl->entries[i]);
  cai_free_mem(NULL, impl->entries);
  impl->entries = NULL;
  impl->count = 0U;
  cai_free_mem(NULL, impl->origin);
  impl->origin = NULL;
}
void cai_config_close(cai_config *self) {
  config_impl *impl;
  size_t i;
  if (self == NULL)
    return;
  impl = IMPL(self);
  if (impl != NULL) {
    entries_cleanup(impl);
    for (i = 0U; i < impl->field_count; i++)
      field_cleanup(&impl->fields[i]);
    cai_free_mem(NULL, impl->fields);
    if (impl->numeric_locale != (locale_t)0)
      freelocale(impl->numeric_locale);
    cai_free_mem(NULL, impl);
  }
  cai_free_mem(NULL, self);
}
static int section_matches(const char *pattern, const char *section) {
  size_t n = strlen(pattern);
  const unsigned char *p;
  if (strcmp(pattern, section) == 0)
    return 1;
  if (n < 3U || strcmp(pattern + n - 2U, ".*") != 0 ||
      strncmp(pattern, section, n - 1U) != 0)
    return 0;
  p = (const unsigned char *)section + n - 1U;
  if (*p == '\0')
    return 0;
  for (; *p != '\0'; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
      return 0;
  return 1;
}

static const cai_config_field *
find_field(const config_impl *impl, const char *section, const char *key) {
  size_t i;
  for (i = 0U; i < impl->field_count; i++)
    if (section_matches(impl->fields[i].field.section, section) &&
        strcmp(impl->fields[i].field.key, key) == 0)
      return &impl->fields[i].field;
  return NULL;
}
static config_entry *find_entry(const config_impl *impl, const char *section,
                                const char *key) {
  size_t i;
  for (i = 0U; i < impl->count; i++)
    if (strcmp(impl->entries[i].section, section) == 0 &&
        strcmp(impl->entries[i].key, key) == 0)
      return &impl->entries[i];
  return NULL;
}
static int name_valid(const char *name, int env) {
  const unsigned char *p = (const unsigned char *)name;
  if (*p == '\0')
    return 0;
  if (env && !(isalpha(*p) || *p == '_'))
    return 0;
  for (; *p; p++)
    if (!(isalnum(*p) || *p == '_' || (!env && (*p == '.' || *p == '-'))))
      return 0;
  return 1;
}
static int section_pattern_valid(const char *name) {
  size_t i, n = strlen(name);
  if (name_valid(name, 0))
    return 1;
  if (n < 3U || strcmp(name + n - 2U, ".*") != 0)
    return 0;
  for (i = 0U; i < n - 2U; i++)
    if (!(isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '-' ||
          name[i] == '.'))
      return 0;
  return 1;
}
static int choice_contains(const char *choices, const char *value, size_t n) {
  const char *p, *end;
  if (choices == NULL)
    return 1;
  p = choices;
  while (*p) {
    end = strchr(p, '|');
    if (end == NULL)
      end = p + strlen(p);
    if ((size_t)(end - p) == n && memcmp(p, value, n) == 0)
      return 1;
    p = *end ? end + 1 : end;
  }
  return 0;
}
static int choice_valid(const char *choices, const char *value) {
  return value[0] == '\0' || choice_contains(choices, value, strlen(value));
}
static int choices_subset(const char *native, const char *choices) {
  const char *p, *end;
  if (native == NULL || choices == NULL)
    return 1;
  for (p = choices; *p; p = *end ? end + 1 : end) {
    end = strchr(p, '|');
    if (end == NULL)
      end = p + strlen(p);
    if (end == p || !choice_contains(native, p, (size_t)(end - p)))
      return 0;
  }
  return *choices != '\0' && choices[strlen(choices) - 1U] != '|';
}
static int validate_value(const config_impl *impl,
                          const cai_config_field *field, const char *value) {
  char *end;
  long long integer;
  double real;
  if (field->section != NULL && value[0] == '\0' &&
      ((strcmp(field->section, "client") == 0 &&
        (strcmp(field->key, "provider") == 0 ||
         strcmp(field->key, "api_protocol") == 0)) ||
       (strcmp(field->section, "response") == 0 &&
        strcmp(field->key, "session_continuity") == 0)))
    return 0;
  if (!choice_valid(field->choices, value))
    return 0;
  if (field->type == CAI_CONFIG_BOOL)
    return strcmp(value, "true") == 0 || strcmp(value, "false") == 0;
  if (field->type == CAI_CONFIG_INTEGER) {
    if (*value == '\0' || isspace((unsigned char)*value))
      return 0;
    errno = 0;
    integer = strtoll(value, &end, 10);
    return errno == 0 && *end == '\0' &&
           ((field->flags & CAI_CONFIG_SIGNED) != 0U || integer >= 0LL);
  }
  if (field->type == CAI_CONFIG_REAL) {
    if (*value == '\0' || isspace((unsigned char)*value))
      return 0;
    errno = 0;
    real = strtod_l(value, &end, impl->numeric_locale);
    return errno == 0 && *end == '\0' && isfinite(real) && real >= 0.0;
  }
  return 1;
}
static int binding_value_valid(const char *section, const char *key,
                               const char *value) {
  size_t i;
  long long n;
  for (i = 0U; i < sizeof(globals) / sizeof(globals[0]); i++)
    if (strcmp(globals[i].section, section) == 0 &&
        strcmp(globals[i].key, key) == 0 &&
        !choice_valid(globals[i].choices, value))
      return 0;
  for (i = 0U; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
    const config_binding *b = &bindings[i];
    if (strcmp(b->section, section) != 0 || strcmp(b->key, key) != 0)
      continue;
    if (!choice_valid(b->choices, value))
      return 0;
    n = strtoll(value, NULL, 10);
    if ((b->storage == S_INT || b->storage == S_LONG || b->storage == S_UINT ||
         b->storage == S_SIZE || b->storage == S_I64) &&
        n < 0LL && (b->flags & CAI_CONFIG_SIGNED) == 0U)
      return 0;
    if (b->storage == S_INT && n > INT_MAX)
      return 0;
    if (b->storage == S_LONG && n > LONG_MAX)
      return 0;
    if (b->storage == S_UINT && (unsigned long long)n > UINT_MAX)
      return 0;
    if (b->storage == S_SIZE && (unsigned long long)n > (size_t)-1)
      return 0;
    if (strcmp(key, "chat_max_attempts") == 0 && n > 16LL)
      return 0;
    if (strcmp(key, "chat_retry_max_delay_ms") == 0 && n > 60000LL)
      return 0;
    if (strcmp(key, "compact_threshold_percent") == 0 && n > 100LL)
      return 0;
  }
  return 1;
}
static int put_value(config_impl *impl, const char *section, const char *key,
                     const char *value, unsigned long line, int replace,
                     cai_error *error) {
  const cai_config_field *field = find_field(impl, section, key);
  cai_config_field env_field;
  config_entry *entry = find_entry(impl, section, key), fresh;
  char *copy;
  char **values;
  config_entry *entries;
  if (strchr(section, '*') != NULL)
    return config_error(error, impl->origin, line, section, key,
                        "named section requires an instance name");
  if (strcmp(section, "env") == 0) {
    if (!name_valid(key, 1))
      return config_error(error, impl->origin, line, section, key,
                          "invalid environment variable name");
    memset(&env_field, 0, sizeof(env_field));
    env_field.type = CAI_CONFIG_STRING;
    field = &env_field;
  }
  if (field == NULL)
    return config_error(error, impl->origin, line, section, key,
                        "unknown setting");
  if (strlen(value) > CONFIG_LINE_MAX)
    return config_error(error, impl->origin, line, section, key,
                        "configuration value exceeds limit");
  if (!validate_value(impl, field, value) ||
      !binding_value_valid(section, key, value))
    return config_error(error, impl->origin, line, section, key,
                        "invalid value for declared type or allowed range");
  if (entry != NULL && !replace && field->type != CAI_CONFIG_STRINGS)
    return config_error(error, impl->origin, line, section, key,
                        "duplicate scalar setting");
  if (entry == NULL && impl->count >= CONFIG_ENTRY_MAX)
    return config_error(error, impl->origin, line, section, key,
                        "too many settings");
  copy = cai_strdup(NULL, value);
  if (copy == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "copy configuration value");
  if (entry == NULL) {
    memset(&fresh, 0, sizeof(fresh));
    fresh.section = cai_strdup(NULL, section);
    fresh.key = cai_strdup(NULL, key);
    fresh.values = cai_alloc(NULL, sizeof(char *));
    if (fresh.section == NULL || fresh.key == NULL || fresh.values == NULL) {
      entry_cleanup(&fresh);
      cai_free_mem(NULL, copy);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "allocate configuration entry");
    }
    fresh.values[0] = copy;
    fresh.count = 1U;
    fresh.line = line;
    entries = cai_realloc_mem(NULL, impl->entries,
                              (impl->count + 1U) * sizeof(*entries));
    if (entries == NULL) {
      entry_cleanup(&fresh);
      return cai_set_error(error, CAI_ERR_NOMEM, "grow configuration entries");
    }
    impl->entries = entries;
    impl->entries[impl->count++] = fresh;
  } else if (field->type == CAI_CONFIG_STRINGS) {
    if (entry->count >= CONFIG_ENTRY_MAX) {
      cai_free_mem(NULL, copy);
      return config_error(error, impl->origin, line, section, key,
                          "too many list entries");
    }
    values = cai_realloc_mem(NULL, entry->values,
                             (entry->count + 1U) * sizeof(char *));
    if (values == NULL) {
      cai_free_mem(NULL, copy);
      return cai_set_error(error, CAI_ERR_NOMEM, "grow configuration list");
    }
    entry->values = values;
    values[entry->count++] = copy;
  } else {
    cai_free_mem(NULL, entry->values[0]);
    entry->values[0] = copy;
    entry->line = line;
  }
  return CAI_OK;
}
const char *cai_config_get(const cai_config *self, const char *section,
                           const char *key) {
  config_entry *entry;
  if (self == NULL || section == NULL || key == NULL)
    return NULL;
  entry = find_entry(IMPL(self), section, key);
  return entry != NULL ? entry->values[entry->count - 1U] : NULL;
}
const char *cai_config_section_at(const cai_config *self, size_t index) {
  const config_impl *impl;
  size_t i, j;
  if (self == NULL)
    return NULL;
  impl = IMPL(self);
  for (i = 0U; i < impl->count; i++) {
    for (j = 0U; j < i; j++)
      if (strcmp(impl->entries[i].section, impl->entries[j].section) == 0)
        break;
    if (j == i && index-- == 0U)
      return impl->entries[i].section;
  }
  return NULL;
}
size_t cai_config_section_count(const cai_config *self) {
  size_t count = 0U, i, j;
  const config_impl *impl;
  if (self == NULL)
    return 0U;
  impl = IMPL(self);
  for (i = 0U; i < impl->count; i++) {
    for (j = 0U; j < i; j++)
      if (strcmp(impl->entries[i].section, impl->entries[j].section) == 0)
        break;
    if (j == i)
      count++;
  }
  return count;
}
const char *const *cai_config_get_list(const cai_config *self,
                                       const char *section, const char *key,
                                       size_t *count) {
  config_entry *entry = self != NULL && section != NULL && key != NULL
                            ? find_entry(IMPL(self), section, key)
                            : NULL;
  if (count != NULL)
    *count = entry != NULL ? entry->count : 0U;
  return entry != NULL ? (const char *const *)entry->values : NULL;
}
int cai_config_set(cai_config *self, const char *section, const char *key,
                   const char *value, cai_error *error) {
  if (self == NULL || section == NULL || key == NULL || value == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration, section, key and value are required");
  return put_value(IMPL(self), section, key, value, 0UL, 1, error);
}
int cai_config_get_bool(const cai_config *self, const char *section,
                        const char *key, int fallback) {
  const char *value = cai_config_get(self, section, key);
  return value != NULL ? strcmp(value, "true") == 0 : fallback;
}
long long cai_config_get_integer(const cai_config *self, const char *section,
                                 const char *key, long long fallback) {
  const char *value = cai_config_get(self, section, key);
  return value != NULL ? strtoll(value, NULL, 10) : fallback;
}
double cai_config_get_real(const cai_config *self, const char *section,
                           const char *key, double fallback) {
  const char *value = cai_config_get(self, section, key);
  return value != NULL ? strtod_l(value, NULL, IMPL(self)->numeric_locale)
                       : fallback;
}
static char *trim(char *text) {
  char *end;
  while (*text && isspace((unsigned char)*text))
    text++;
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1]))
    *--end = '\0';
  return text;
}
static int parse_line(config_impl *impl, char *bytes, char **section,
                      unsigned long line, cai_error *error) {
  char *text = trim(bytes), *key, *value, *end, *write, *copy;
  int quote;
  size_t i;
  if (*text == '\0' || *text == '#' || *text == ';')
    return CAI_OK;
  if (*text == '[') {
    end = strchr(text, ']');
    if (end == NULL)
      return config_error(error, impl->origin, line, *section, NULL,
                          "unterminated section");
    *end = '\0';
    key = trim(text + 1);
    value = trim(end + 1);
    if (!name_valid(key, 0) || (*value && *value != '#' && *value != ';'))
      return config_error(error, impl->origin, line, *section, NULL,
                          "invalid section declaration");
    if (strcmp(key, "env") != 0) {
      for (i = 0U; i < impl->field_count; i++)
        if (section_matches(impl->fields[i].field.section, key))
          break;
      if (i == impl->field_count)
        return config_error(error, impl->origin, line, key, NULL,
                            "unknown section");
    }
    copy = cai_strdup(NULL, key);
    if (copy == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "copy section");
    cai_free_mem(NULL, *section);
    *section = copy;
    return CAI_OK;
  }
  end = strchr(text, '=');
  if (end == NULL)
    return config_error(error, impl->origin, line, *section, NULL,
                        "expected key = value");
  *end = '\0';
  key = trim(text);
  value = trim(end + 1);
  if (!name_valid(key, strcmp(*section, "env") == 0))
    return config_error(error, impl->origin, line, *section, NULL,
                        "invalid key name");
  if (*value == '"' || *value == '\'') {
    quote = *value++;
    write = value;
    text = value;
    while (*text && *text != quote) {
      if (*text == '\\') {
        text++;
        if (*text == 'n')
          *write++ = '\n';
        else if (*text == 'r')
          *write++ = '\r';
        else if (*text == 't')
          *write++ = '\t';
        else if (*text == '\\' || *text == '"' || *text == '\'')
          *write++ = *text;
        else
          return config_error(error, impl->origin, line, *section, key,
                              "invalid quoted escape");
        if (*text)
          text++;
      } else
        *write++ = *text++;
    }
    if (*text != quote)
      return config_error(error, impl->origin, line, *section, key,
                          "unterminated quoted value");
    text = trim(text + 1);
    if (*text && *text != '#' && *text != ';')
      return config_error(error, impl->origin, line, *section, key,
                          "unexpected text after quoted value");
    *write = '\0';
  }
  return put_value(impl, *section, key, value, line, 0, error);
}
int cai_config_load(cai_config *self, cai_source *source, const char *origin,
                    cai_error *error) {
  config_impl next, *impl;
  cai_buffer_builder line;
  char empty[] = "";
  char bytes[4096], *section;
  size_t n, i;
  unsigned long number = 1UL;
  int rc = CAI_OK;
  cai_error local_error;
  if (error == NULL) {
    cai_error_init(&local_error);
    rc = cai_config_load(self, source, origin, &local_error);
    cai_error_cleanup(&local_error);
    return rc;
  }
  if (self == NULL || source == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration and source are required");
  impl = IMPL(self);
  memset(&next, 0, sizeof(next));
  memset(&line, 0, sizeof(line));
  next.fields = impl->fields;
  next.field_count = impl->field_count;
  next.numeric_locale = impl->numeric_locale;
  next.origin = cai_strdup(NULL, origin != NULL ? origin : "configuration");
  section = cai_strdup(NULL, "");
  if (next.origin == NULL || section == NULL)
    rc = cai_set_error(error, CAI_ERR_NOMEM, "allocate configuration parser");
  while (rc == CAI_OK &&
         (n = cai_source_read(source, bytes, sizeof(bytes), error)) != 0U) {
    for (i = 0U; i < n && rc == CAI_OK; i++) {
      if (bytes[i] == '\0') {
        rc = config_error(error, next.origin, number, section, NULL,
                          "NUL byte in configuration");
        break;
      }
      if (bytes[i] == '\n') {
        rc = parse_line(&next, line.data != NULL ? line.data : empty, &section,
                        number, error);
        line.length = 0U;
        if (line.data != NULL)
          line.data[0] = '\0';
        number++;
      } else if (line.length >= CONFIG_LINE_MAX)
        rc = config_error(error, next.origin, number, section, NULL,
                          "configuration line exceeds limit");
      else
        rc = cai_buffer_append(&line, bytes + i, 1U, error);
    }
  }
  if (rc == CAI_OK && error != NULL && error->code != CAI_OK)
    rc = error->code;
  if (rc == CAI_OK && line.length != 0U)
    rc = parse_line(&next, line.data, &section, number, error);
  if (rc == CAI_OK) {
    entries_cleanup(impl);
    impl->entries = next.entries;
    impl->count = next.count;
    impl->origin = next.origin;
    next.entries = NULL;
    next.count = 0U;
    next.origin = NULL;
  }
  entries_cleanup(&next);
  cai_free_mem(NULL, section);
  cai_free_mem(NULL, line.data);
  return rc;
}
int cai_config_load_file(cai_config *self, const char *path, cai_error *error) {
  FILE *file;
  cai_source *source = NULL;
  struct stat status;
  char cwd[4096];
  char *absolute = NULL;
  size_t length;
  int fd, rc;
  if (self == NULL || path == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration and filename are required");
  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return config_error(error, path, 0UL, "", NULL,
                        "cannot open configuration file");
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    close(fd);
    return config_error(error, path, 0UL, "", NULL,
                        "configuration must be a regular file");
  }
  file = fdopen(fd, "rb");
  if (file == NULL) {
    close(fd);
    return config_error(error, path, 0UL, "", NULL,
                        "cannot open configuration stream");
  }
  rc = cai_source_file(file, 1, &source, error);
  if (rc != CAI_OK)
    fclose(file);
  else {
    if (path[0] != '/') {
      if (getcwd(cwd, sizeof(cwd)) == NULL)
        rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                           "resolve configuration file directory");
      else {
        length = strlen(cwd) + strlen(path) + 2U;
        absolute = cai_alloc(NULL, length);
        if (absolute == NULL)
          rc = cai_set_error(error, CAI_ERR_NOMEM,
                             "resolve configuration filename");
        else
          snprintf(absolute, length, "%s/%s", cwd, path);
      }
    }
    if (rc == CAI_OK)
      rc = cai_config_load(self, source, absolute != NULL ? absolute : path,
                           error);
    cai_source_close(source);
    cai_free_mem(NULL, absolute);
  }
  return rc;
}

static cai_config_type storage_type(config_storage storage) {
  if (storage == S_STR || storage == S_PROTOCOL || storage == S_CONTINUITY)
    return CAI_CONFIG_STRING;
  if (storage == S_BOOL || storage == S_NOTBOOL)
    return CAI_CONFIG_BOOL;
  if (storage == S_REAL)
    return CAI_CONFIG_REAL;
  return CAI_CONFIG_INTEGER;
}
static int add_field(config_impl *impl, const cai_config_field *field,
                     cai_error *error) {
  config_owned_field copy, *fields;
  size_t index, i;
  const char *inputs[5];
  const char *native_choices = NULL;
  if (field->section == NULL || field->key == NULL ||
      !name_valid(field->key, 0) ||
      (field->section[0] != '\0' && !section_pattern_valid(field->section)) ||
      strcmp(field->section, "env") == 0 || field->type < CAI_CONFIG_STRING ||
      field->type > CAI_CONFIG_STRINGS ||
      (field->flags &
       ~(CAI_CONFIG_PATH | CAI_CONFIG_SECRET | CAI_CONFIG_SIGNED)) != 0U ||
      ((field->flags & CAI_CONFIG_PATH) != 0U &&
       field->type != CAI_CONFIG_STRING) ||
      ((field->flags & CAI_CONFIG_SIGNED) != 0U &&
       field->type != CAI_CONFIG_INTEGER) ||
      ((field->flags & CAI_CONFIG_SECRET) != 0U &&
       field->type != CAI_CONFIG_STRING && field->type != CAI_CONFIG_STRINGS))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid configuration schema field");
  if (field->default_value != NULL &&
      (!validate_value(impl, field, field->default_value) ||
       !binding_value_valid(field->section, field->key, field->default_value)))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid configuration template default");
  memset(&copy, 0, sizeof(copy));
  copy.field = *field;
  for (i = 0U; i < sizeof(globals) / sizeof(globals[0]); i++) {
    if (strcmp(field->section, globals[i].section) == 0 &&
        strcmp(field->key, globals[i].key) == 0) {
      if (field->type != globals[i].type)
        return cai_set_error(
            error, CAI_ERR_INVALID,
            "configuration schema changes a standard field type");
      native_choices = globals[i].choices;
    }
  }
  for (i = 0U; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
    const config_binding *b = &bindings[i];
    if (strcmp(field->section, b->section) == 0 &&
        strcmp(field->key, b->key) == 0) {
      if (field->type != storage_type(b->storage))
        return cai_set_error(
            error, CAI_ERR_INVALID,
            "configuration schema changes a standard field type");
      copy.field.flags = (copy.field.flags & ~CAI_CONFIG_SIGNED) | b->flags;
      native_choices = b->choices;
    }
  }
  if (!choices_subset(native_choices, field->choices))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration schema widens standard field choices");
  inputs[0] = field->section;
  inputs[1] = field->key;
  inputs[2] = field->default_value;
  inputs[3] = field->description;
  inputs[4] = field->choices != NULL ? field->choices : native_choices;
  for (index = 0U; index < 5U; index++) {
    copy.strings[index] = cai_strdup(NULL, inputs[index]);
    if (inputs[index] != NULL && copy.strings[index] == NULL) {
      field_cleanup(&copy);
      return cai_set_error(error, CAI_ERR_NOMEM, "copy configuration schema");
    }
  }
  copy.field.section = copy.strings[0];
  copy.field.key = copy.strings[1];
  copy.field.default_value = copy.strings[2];
  copy.field.description = copy.strings[3];
  copy.field.choices = copy.strings[4];
  for (index = 0U; index < impl->field_count; index++)
    if (strcmp(impl->fields[index].field.section, field->section) == 0 &&
        strcmp(impl->fields[index].field.key, field->key) == 0)
      break;
  if (index < impl->field_count) {
    if (impl->fields[index].field.type != field->type) {
      field_cleanup(&copy);
      return cai_set_error(
          error, CAI_ERR_INVALID,
          "configuration extension changes a standard field type");
    }
    field_cleanup(&impl->fields[index]);
    impl->fields[index] = copy;
  } else {
    fields = cai_realloc_mem(NULL, impl->fields,
                             (impl->field_count + 1U) * sizeof(*fields));
    if (fields == NULL) {
      field_cleanup(&copy);
      return cai_set_error(error, CAI_ERR_NOMEM, "grow configuration schema");
    }
    impl->fields = fields;
    impl->fields[impl->field_count++] = copy;
  }
  return CAI_OK;
}
int cai_config_set_default(cai_config *self, const char *section,
                           const char *key, const char *value,
                           cai_error *error) {
  const cai_config_field *existing;
  cai_config_field changed;
  if (self == NULL || section == NULL || key == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration, section and key required");
  existing = find_field(IMPL(self), section, key);
  if (existing == NULL)
    return config_error(error, NULL, 0UL, section, key,
                        "unknown template field");
  changed = *existing;
  changed.default_value = value;
  return add_field(IMPL(self), &changed, error);
}
size_t cai_config_field_count(const cai_config *self) {
  return self != NULL ? IMPL(self)->field_count : 0U;
}
const cai_config_field *cai_config_field_at(const cai_config *self,
                                            size_t index) {
  return self != NULL && index < IMPL(self)->field_count
             ? &IMPL(self)->fields[index].field
             : NULL;
}
static int config_open(const cai_config_field *extensions, size_t count,
                       int sdk_schema, cai_config **out, cai_error *error) {
  cai_config *self;
  config_impl *impl;
  cai_client_config client;
  cai_agent_config agent;
  cai_agent_runtime_config runtime;
  cai_terminal_tool_config terminal;
  cai_config_field field;
  char text[128];
  const char *value;
  const void *record;
  const unsigned char *ptr;
  size_t i;
  int rc;
  if (out == NULL || (count != 0U && extensions == NULL))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration output/schema required");
  *out = NULL;
  self = cai_alloc(NULL, sizeof(*self));
  impl = cai_alloc(NULL, sizeof(*impl));
  if (self == NULL || impl == NULL) {
    cai_free_mem(NULL, self);
    cai_free_mem(NULL, impl);
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate configuration");
  }
  memset(self, 0, sizeof(*self));
  memset(impl, 0, sizeof(*impl));
  self->impl = impl;
  impl->numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (impl->numeric_locale == (locale_t)0) {
    cai_config_close(self);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "create configuration numeric locale");
  }
  self->field_count = cai_config_field_count;
  self->section_count = cai_config_section_count;
  self->section_at = cai_config_section_at;
  self->field_at = cai_config_field_at;
  self->load = cai_config_load;
  self->load_file = cai_config_load_file;
  self->get_bool = cai_config_get_bool;
  self->get_integer = cai_config_get_integer;
  self->get_real = cai_config_get_real;
  self->get_list = cai_config_get_list;
  self->resolve_path = cai_config_resolve_path;
  self->set_default = cai_config_set_default;
  self->get = cai_config_get;
  self->set = cai_config_set;
  self->apply_environment = cai_config_apply_environment;
  self->write_template = cai_config_write_template;
  self->create_template_file = cai_config_create_template_file;
  self->apply_client = cai_config_apply_client;
  self->apply_agent = cai_config_apply_agent;
  self->apply_runtime = cai_config_apply_runtime;
  self->apply_terminal = cai_config_apply_terminal;
  self->close = cai_config_close;
  cai_client_config_init(&client);
  cai_agent_config_init(&agent);
  cai_agent_runtime_config_init(&runtime);
  memset(&terminal, 0, sizeof(terminal));
  rc = CAI_OK;
  for (i = 0U;
       sdk_schema && rc == CAI_OK && i < sizeof(globals) / sizeof(globals[0]);
       i++)
    rc = add_field(impl, &globals[i], error);
  for (i = 0U;
       sdk_schema && rc == CAI_OK && i < sizeof(bindings) / sizeof(bindings[0]);
       i++) {
    const config_binding *b = &bindings[i];
    record = b->target == T_CLIENT    ? (const void *)&client
             : b->target == T_AGENT   ? (const void *)&agent
             : b->target == T_RUNTIME ? (const void *)&runtime
                                      : (const void *)&terminal;
    ptr = (const unsigned char *)record + b->offset;
    value = text;
    if (b->storage == S_STR)
      value = *(const char *const *)ptr;
    else if (b->storage == S_BOOL || b->storage == S_NOTBOOL)
      value = (*(const int *)ptr != 0) ^ (b->storage == S_NOTBOOL) ? "true"
                                                                   : "false";
    else if (b->storage == S_PROTOCOL)
      value = "responses";
    else if (b->storage == S_CONTINUITY)
      value = "auto";
    else if (b->storage == S_INT)
      snprintf(text, sizeof(text), "%d", *(const int *)ptr);
    else if (b->storage == S_LONG)
      snprintf(text, sizeof(text), "%ld", *(const long *)ptr);
    else if (b->storage == S_UINT)
      snprintf(text, sizeof(text), "%u", *(const unsigned int *)ptr);
    else if (b->storage == S_SIZE)
      snprintf(text, sizeof(text), "%lu", (unsigned long)*(const size_t *)ptr);
    else if (b->storage == S_I64)
      snprintf(text, sizeof(text), "%lld", *(const long long *)ptr);
    else
      snprintf(text, sizeof(text), "%.17g", *(const double *)ptr);
    memset(&field, 0, sizeof(field));
    field.section = b->section;
    field.key = b->key;
    field.type = storage_type(b->storage);
    field.default_value = value;
    field.description = b->description;
    field.choices = b->choices;
    field.flags = b->flags;
    rc = add_field(impl, &field, error);
  }
  for (i = 0U; rc == CAI_OK && i < count; i++)
    rc = add_field(impl, &extensions[i], error);
  if (rc != CAI_OK) {
    cai_config_close(self);
    return rc;
  }
  *out = self;
  return CAI_OK;
}
int cai_config_open(const cai_config_field *extensions, size_t count,
                    cai_config **out, cai_error *error) {
  return config_open(extensions, count, 1, out, error);
}
int cai_config_open_schema(const cai_config_field *fields, size_t count,
                           cai_config **out, cai_error *error) {
  return config_open(fields, count, 0, out, error);
}
static int write_literal(cai_sink *sink, const char *text, cai_error *error) {
  return cai_sink_write(sink, text, strlen(text), error);
}
static int write_comment(cai_sink *sink, const char *text, cai_error *error) {
  const char *p, *start = text;
  int rc = CAI_OK;
  for (p = text; rc == CAI_OK && *p; p++) {
    if (*p == '\n' || *p == '\r') {
      rc = cai_sink_write(sink, start, (size_t)(p - start), error);
      if (rc == CAI_OK)
        rc = write_literal(sink, "\n## ", error);
      start = p + 1;
    }
  }
  return rc == CAI_OK ? write_literal(sink, start, error) : rc;
}
static int write_quoted(cai_sink *sink, const char *value, cai_error *error) {
  int rc = write_literal(sink, "\"", error);
  const char *p;
  for (p = value; rc == CAI_OK && *p; p++) {
    const char *escape = *p == '\n'   ? "\\n"
                         : *p == '\r' ? "\\r"
                         : *p == '\t' ? "\\t"
                         : *p == '\\' ? "\\\\"
                         : *p == '"'  ? "\\\""
                                      : NULL;
    rc = escape != NULL ? write_literal(sink, escape, error)
                        : cai_sink_write(sink, p, 1U, error);
  }
  return rc == CAI_OK ? write_literal(sink, "\"", error) : rc;
}
int cai_config_write_template(const cai_config *self, cai_sink *sink,
                              cai_error *error) {
  const config_impl *impl;
  const char *section = "";
  size_t i, j, pass;
  int rc;
  if (self == NULL || sink == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration and sink required");
  impl = IMPL(self);
  rc = write_literal(
      sink,
      "## CAI configuration. Uncomment a single-# assignment to set it "
      "explicitly.\n## "
      "Quoted values support \\n, \\r, \\t, \\\\ and \\\". Comments start with "
      "# or ; on their own line or after a quoted value.\n## Empty strings "
      "have the field-specific meaning documented below; "
      "zero limits use\n## the documented automatic default or disable that "
      "limit. Paths support ~/\n## and file-relative paths. Existing "
      "environment wins unless changed below.\n\n",
      error);
  /* Global assignments must precede every section regardless of schema order.
   */
  for (pass = 0U; rc == CAI_OK && pass < 2U; pass++) {
    for (i = 0U; rc == CAI_OK && i < impl->field_count; i++) {
      const cai_config_field *f = &impl->fields[i].field;
      if ((f->section[0] == '\0') != (pass == 0U))
        continue;
      /* Group declarations by first occurrence of section, not table adjacency.
       */
      for (j = 0U; j < i; j++)
        if (strcmp(impl->fields[j].field.section, f->section) == 0)
          break;
      if (j < i)
        continue;
      section = f->section;
      if (section[0] != '\0') {
        size_t n = strlen(section);
        int named = n > 2U && strcmp(section + n - 2U, ".*") == 0;
        rc = write_literal(
            sink,
            named ? "\n## Copy and name each server section.\n# [" : "\n[",
            error);
        if (rc == CAI_OK)
          rc = named ? cai_sink_write(sink, section, n - 1U, error)
                     : write_literal(sink, section, error);
        if (rc == CAI_OK && named)
          rc = write_literal(sink, "example", error);
        if (rc == CAI_OK)
          rc = write_literal(sink, "]\n", error);
      }
      for (j = i; rc == CAI_OK && j < impl->field_count; j++) {
        const cai_config_field *entry = &impl->fields[j].field;
        const char *value = entry->default_value;
        if (strcmp(entry->section, section) != 0)
          continue;
        rc = write_literal(sink, "## ", error);
        if (rc == CAI_OK)
          rc = write_comment(sink,
                             entry->description != NULL ? entry->description
                                                        : "Optional setting.",
                             error);
        if (rc == CAI_OK && entry->choices != NULL) {
          rc = write_literal(sink, " Choices: ", error);
          if (rc == CAI_OK)
            rc = write_comment(sink, entry->choices, error);
        }
        if (rc == CAI_OK)
          rc = write_literal(sink, "\n# ", error);
        if (rc == CAI_OK)
          rc = write_literal(sink, entry->key, error);
        if (rc == CAI_OK)
          rc = write_literal(sink, " = ", error);
        if ((entry->flags & CAI_CONFIG_SECRET) != 0U)
          value = NULL;
        if (rc == CAI_OK) {
          if (value == NULL || entry->type == CAI_CONFIG_STRING ||
              entry->type == CAI_CONFIG_STRINGS)
            rc = write_quoted(sink, value != NULL ? value : "", error);
          else
            rc = write_literal(sink, value, error);
        }
        if (rc == CAI_OK)
          rc = write_literal(sink, "\n", error);
      }
    }
  }
  if (rc == CAI_OK)
    rc = write_literal(
        sink,
        "\n[env]\n## Optional literal environment assignments; values are "
        "never "
        "interpolated.\n## This section does not copy your environment or "
        "credentials. The assignments below are examples, not defaults.\n"
        "# BERGET_API_KEY = \"\"\n# LOG_LEVEL = \"info\"\n",
        error);
  return rc;
}
int cai_config_resolve_path(cai_config *self, const char *section,
                            const char *key, const char **out,
                            cai_error *error) {
  config_entry *entry;
  const cai_config_field *field;
  const char *value, *home, *slash;
  char cwd[4096];
  char *path;
  size_t length, base;
  if (out == NULL || self == NULL || section == NULL || key == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "path arguments required");
  *out = NULL;
  field = find_field(IMPL(self), section, key);
  if (field == NULL || (field->flags & CAI_CONFIG_PATH) == 0U ||
      field->type != CAI_CONFIG_STRING)
    return config_error(error, IMPL(self)->origin, 0UL, section, key,
                        "setting is not a declared path");
  entry = find_entry(IMPL(self), section, key);
  if (entry == NULL || entry->values[0][0] == '\0')
    return CAI_OK;
  value = entry->values[0];
  if (value[0] == '/') {
    *out = value;
    return CAI_OK;
  }
  if (value[0] == '~' && (value[1] == '/' || value[1] == '\0')) {
    home = getenv("HOME");
    if (home == NULL || home[0] != '/')
      return config_error(error, IMPL(self)->origin, entry->line, section, key,
                          "HOME must be absolute for ~ paths");
    length = strlen(home) + strlen(value) + 1U;
    path = cai_alloc(NULL, length);
    if (path != NULL)
      snprintf(path, length, "%s%s", home, value + 1);
  } else {
    slash = IMPL(self)->origin != NULL && IMPL(self)->origin[0] == '/'
                ? strrchr(IMPL(self)->origin, '/')
                : NULL;
    if (slash != NULL)
      base = (size_t)(slash - IMPL(self)->origin);
    else {
      if (getcwd(cwd, sizeof(cwd)) == NULL)
        return cai_set_error(error, CAI_ERR_TRANSPORT,
                             "resolve configuration working directory");
      base = strlen(cwd);
    }
    length = base + strlen(value) + 2U;
    path = cai_alloc(NULL, length);
    if (path != NULL) {
      memcpy(path, slash != NULL ? IMPL(self)->origin : cwd, base);
      path[base] = '/';
      strcpy(path + base + 1U, value);
    }
  }
  if (path == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "resolve configuration path");
  cai_free_mem(NULL, entry->values[0]);
  entry->values[0] = path;
  *out = path;
  return CAI_OK;
}
int cai_config_apply_environment(const cai_config *self, cai_error *error) {
  const config_impl *impl;
  char **previous;
  unsigned char *change;
  size_t i, j;
  int override, rc = CAI_OK;
  if (self == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "configuration required");
  impl = IMPL(self);
  override = cai_config_get_bool(self, "", "config_env_has_precedence", 0);
  previous =
      cai_alloc(NULL, (impl->count != 0U ? impl->count : 1U) * sizeof(char *));
  change = cai_alloc(NULL, impl->count != 0U ? impl->count : 1U);
  if (previous == NULL || change == NULL) {
    cai_free_mem(NULL, previous);
    cai_free_mem(NULL, change);
    return cai_set_error(error, CAI_ERR_NOMEM, "prepare environment changes");
  }
  memset(previous, 0, impl->count * sizeof(char *));
  memset(change, 0, impl->count);
  for (i = 0U; i < impl->count; i++) {
    const char *value;
    if (strcmp(impl->entries[i].section, "env") != 0)
      continue;
    value = getenv(impl->entries[i].key);
    if (value != NULL && !override)
      continue;
    change[i] = 1;
    if (value != NULL) {
      previous[i] = cai_strdup(NULL, value);
      if (previous[i] == NULL) {
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "snapshot environment for configuration");
        break;
      }
    }
  }
  if (rc == CAI_OK)
    for (i = 0U; i < impl->count; i++)
      if (change[i] &&
          setenv(impl->entries[i].key, impl->entries[i].values[0], 1) != 0) {
        for (j = 0U; j < i; j++)
          if (change[j]) {
            if (previous[j] != NULL)
              (void)setenv(impl->entries[j].key, previous[j], 1);
            else
              (void)unsetenv(impl->entries[j].key);
          }
        rc = config_error(error, impl->origin, impl->entries[i].line, "env",
                          impl->entries[i].key, "cannot set process variable");
        break;
      }
  for (i = 0U; i < impl->count; i++)
    cai_free_mem(NULL, previous[i]);
  cai_free_mem(NULL, previous);
  cai_free_mem(NULL, change);
  return rc;
}
static int apply_record(const cai_config *self, config_target target, void *out,
                        cai_error *error) {
  size_t i;
  const char *value;
  unsigned char *ptr;
  if (self == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "configuration and target record required");
  for (i = 0U; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
    const config_binding *b = &bindings[i];
    if (b->target != target)
      continue;
    value = cai_config_get(self, b->section, b->key);
    if (value == NULL)
      continue;
    ptr = (unsigned char *)out + b->offset;
    switch (b->storage) {
    case S_STR:
      /* Empty provider-dependent transport strings inherit the preset. */
      if (target == T_CLIENT && value[0] == '\0' &&
          (strcmp(b->key, "base_url") == 0 ||
           strcmp(b->key, "api_key_env") == 0 ||
           strcmp(b->key, "metadata_provider") == 0))
        break;
      *(const char **)ptr = value[0] != '\0' ? value : NULL;
      break;
    case S_BOOL:
      *(int *)ptr = strcmp(value, "true") == 0;
      break;
    case S_NOTBOOL:
      *(int *)ptr = strcmp(value, "true") != 0;
      break;
    case S_INT:
      *(int *)ptr = (int)strtoll(value, NULL, 10);
      break;
    case S_LONG:
      *(long *)ptr = (long)strtoll(value, NULL, 10);
      break;
    case S_UINT:
      *(unsigned int *)ptr = (unsigned int)strtoll(value, NULL, 10);
      break;
    case S_SIZE:
      *(size_t *)ptr = (size_t)strtoll(value, NULL, 10);
      break;
    case S_I64:
      *(long long *)ptr = strtoll(value, NULL, 10);
      break;
    case S_REAL:
      *(double *)ptr = strtod_l(value, NULL, IMPL(self)->numeric_locale);
      break;
    case S_PROTOCOL:
      *(cai_api_protocol *)ptr = strcmp(value, "responses") == 0
                                     ? CAI_API_PROTOCOL_RESPONSES
                                     : CAI_API_PROTOCOL_CHAT_COMPLETIONS;
      break;
    case S_CONTINUITY:
      *(int *)ptr = strcmp(value, "auto") == 0 ? CAI_SESSION_CONTINUITY_AUTO
                    : strcmp(value, "client") == 0
                        ? CAI_SESSION_CONTINUITY_CLIENT_HISTORY
                        : CAI_SESSION_CONTINUITY_SERVER;
      break;
    }
  }
  return CAI_OK;
}
int cai_config_apply_client(const cai_config *self, cai_client_config *out,
                            cai_error *error) {
  return apply_record(self, T_CLIENT, out, error);
}
int cai_config_apply_agent(const cai_config *self, cai_agent_config *out,
                           cai_error *error) {
  return apply_record(self, T_AGENT, out, error);
}
int cai_config_apply_terminal(const cai_config *self,
                              cai_terminal_tool_config *out, cai_error *error) {
  return apply_record(self, T_TERMINAL, out, error);
}
int cai_config_apply_runtime(const cai_config *self,
                             cai_agent_runtime_config *out, cai_error *error) {
  int rc = apply_record(self, T_RUNTIME, out, error);
  if (rc != CAI_OK)
    return rc;
  if (cai_config_get(self, "review", "allowed_models") != NULL)
    out->review_allowed_models = cai_config_get_list(
        self, "review", "allowed_models", &out->review_allowed_model_count);
  if (cai_config_get(self, "review", "allowed_reasoning_efforts") != NULL)
    out->review_allowed_reasoning_efforts =
        cai_config_get_list(self, "review", "allowed_reasoning_efforts",
                            &out->review_allowed_reasoning_effort_count);
  if (cai_config_get(self, "review", "allowed_reasoning_summaries") != NULL)
    out->review_allowed_reasoning_summaries =
        cai_config_get_list(self, "review", "allowed_reasoning_summaries",
                            &out->review_allowed_reasoning_summary_count);
  if (cai_config_get(self, "review", "allowed_models") != NULL &&
      out->review_allowed_model_count == 1U &&
      out->review_allowed_models != NULL &&
      out->review_allowed_models[0][0] == '\0') {
    out->review_allowed_models = NULL;
    out->review_allowed_model_count = 0U;
  }
  if (cai_config_get(self, "review", "allowed_reasoning_efforts") != NULL &&
      out->review_allowed_reasoning_effort_count == 1U &&
      out->review_allowed_reasoning_efforts != NULL &&
      out->review_allowed_reasoning_efforts[0][0] == '\0') {
    out->review_allowed_reasoning_efforts = NULL;
    out->review_allowed_reasoning_effort_count = 0U;
  }
  if (cai_config_get(self, "review", "allowed_reasoning_summaries") != NULL &&
      out->review_allowed_reasoning_summary_count == 1U &&
      out->review_allowed_reasoning_summaries != NULL &&
      out->review_allowed_reasoning_summaries[0][0] == '\0') {
    out->review_allowed_reasoning_summaries = NULL;
    out->review_allowed_reasoning_summary_count = 0U;
  }
  return CAI_OK;
}

int cai_config_create_template_file(const cai_config *self, const char *path,
                                    cai_error *error) {
  char *directory, *temporary, *p;
  size_t length;
  FILE *file;
  cai_sink *sink = NULL;
  struct stat status;
  int fd, rc, saved_errno, created;
  if (self == NULL || path == NULL || path[0] != '/')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "absolute configuration filename required");
  if (lstat(path, &status) == 0)
    return CAI_OK;
  if (errno != ENOENT)
    return config_error(error, path, 0UL, "", NULL,
                        "inspect configuration destination");
  directory = cai_strdup(NULL, path);
  if (directory == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "prepare configuration directory");
  p = strrchr(directory, '/');
  if (p == directory)
    p[1] = '\0';
  else
    *p = '\0';
  for (p = directory + 1;; p++) {
    char old = *p;
    if (old != '/' && old != '\0')
      continue;
    *p = '\0';
    created = mkdir(directory, 0700) == 0;
    if (!created && errno != EEXIST) {
      cai_free_mem(NULL, directory);
      return config_error(error, path, 0UL, "", NULL,
                          "create configuration parent directory");
    }
    if (created && chmod(directory, 0700) != 0) {
      cai_free_mem(NULL, directory);
      return config_error(error, path, 0UL, "", NULL,
                          "set private configuration directory permissions");
    }
    if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
      cai_free_mem(NULL, directory);
      return config_error(error, path, 0UL, "", NULL,
                          "configuration parent is not a directory");
    }
    *p = old;
    if (old == '\0')
      break;
  }
  length = strlen(directory) + 32U;
  temporary = cai_alloc(NULL, length);
  if (temporary == NULL) {
    cai_free_mem(NULL, directory);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "prepare configuration template file");
  }
  snprintf(temporary, length, "%s/.config.ini.XXXXXX", directory);
  fd = mkstemp(temporary);
  cai_free_mem(NULL, directory);
  if (fd < 0) {
    cai_free_mem(NULL, temporary);
    return config_error(error, path, 0UL, "", NULL,
                        "create private configuration template");
  }
  if (fchmod(fd, 0600) != 0) {
    close(fd);
    unlink(temporary);
    cai_free_mem(NULL, temporary);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "set private configuration file permissions");
  }
  file = fdopen(fd, "wb");
  if (file == NULL) {
    close(fd);
    unlink(temporary);
    cai_free_mem(NULL, temporary);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "open configuration template stream");
  }
  rc = cai_sink_file(file, 0, &sink, error);
  if (rc == CAI_OK)
    rc = cai_config_write_template(self, sink, error);
  cai_sink_close(sink);
  if (rc == CAI_OK && (fflush(file) != 0 || fsync(fd) != 0))
    rc =
        cai_set_error(error, CAI_ERR_TRANSPORT, "flush configuration template");
  if (fclose(file) != 0 && rc == CAI_OK)
    rc =
        cai_set_error(error, CAI_ERR_TRANSPORT, "close configuration template");
  if (rc == CAI_OK && link(temporary, path) != 0 && errno != EEXIST) {
    saved_errno = errno;
    rc = config_error(error, path, 0UL, "", NULL,
                      saved_errno == EACCES
                          ? "configuration destination is not writable"
                          : "install configuration template");
  }
  unlink(temporary);
  cai_free_mem(NULL, temporary);
  return rc;
}
