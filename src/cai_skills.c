#include "cai_internal.h"

#include <cai/skills.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define CAI_SKILLS_MAX_PACKAGES 256U
#define CAI_SKILLS_MAX_DEPTH 8U
#define CAI_SKILLS_MAX_FILE_BYTES (1024U * 1024U)

typedef struct cai_skill_entry {
  char *id;
  char *name;
  char *description;
} cai_skill_entry;

typedef struct cai_local_skill_provider {
  int root_fd;
} cai_local_skill_provider;

struct cai_skill_catalog {
  cai_skill_provider provider;
  cai_local_skill_provider *local;
  cai_skill_entry *entries;
  size_t count;
  size_t capacity;
  void (*warning_callback)(void *context, const char *message);
  void *warning_context;
};

typedef struct cai_skill_args {
  char *skill;
  char *resource;
} cai_skill_args;

typedef struct cai_skill_result {
  char *skill;
  char *resource;
  lonejson_spooled content;
} cai_skill_result;

static const lonejson_field cai_skill_args_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_skill_args, skill, "skill"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_skill_args, resource,
                                          "resource")};
LONEJSON_MAP_DEFINE(cai_skill_args_map, cai_skill_args, cai_skill_args_fields);

static const lonejson_field cai_skill_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_skill_result, skill, "skill"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_skill_result, resource, "resource"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_skill_result, content, "content")};
LONEJSON_MAP_DEFINE(cai_skill_result_map, cai_skill_result,
                    cai_skill_result_fields);

static const char cai_skill_schema_json[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"skill\":{\"type\":\"string\"},"
    "\"resource\":{\"type\":[\"string\",\"null\"]}},"
    "\"required\":[\"skill\"],\"additionalProperties\":false}";

static void cai_skills_warn(cai_skill_catalog *catalog, const char *message) {
  if (catalog->warning_callback != NULL) {
    catalog->warning_callback(catalog->warning_context, message);
  }
}

void cai_skill_config_init(cai_skill_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

static int cai_skill_segment_valid(const char *value) {
  const unsigned char *cursor;

  if (value == NULL || value[0] == '\0' || strcmp(value, ".") == 0 ||
      strcmp(value, "..") == 0) {
    return 0;
  }
  for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
    if (*cursor == '/' || *cursor == '\\' || *cursor < 0x21U ||
        *cursor == 0x7fU) {
      return 0;
    }
  }
  return 1;
}

static int cai_skill_relative_path_valid(const char *path) {
  const char *cursor;
  const char *slash;
  char segment[256];
  size_t length;

  if (path == NULL || path[0] == '\0' || path[0] == '/') {
    return 0;
  }
  cursor = path;
  while (*cursor != '\0') {
    slash = strchr(cursor, '/');
    length = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
    if (length == 0U || length >= sizeof(segment)) {
      return 0;
    }
    memcpy(segment, cursor, length);
    segment[length] = '\0';
    if (!cai_skill_segment_valid(segment)) {
      return 0;
    }
    if (slash == NULL) {
      return 1;
    }
    cursor = slash + 1;
  }
  return 0;
}

static int cai_skill_id_valid(const char *id) {
  return cai_skill_relative_path_valid(id);
}

static void cai_skill_local_cleanup(void *context) {
  cai_local_skill_provider *local = (cai_local_skill_provider *)context;

  if (local != NULL) {
    if (local->root_fd >= 0) {
      (void)close(local->root_fd);
    }
    cai_free_mem(NULL, local);
  }
}

static int cai_skill_open_relative_file(int root_fd, const char *id,
                                        const char *resource, int *out_fd,
                                        cai_error *error) {
  char component[256];
  const char *cursor;
  const char *slash;
  size_t length;
  int fd;
  int next_fd;
  struct stat st;

  *out_fd = -1;
  if (!cai_skill_relative_path_valid(id) ||
      (resource != NULL && !cai_skill_relative_path_valid(resource))) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill resource path is invalid");
  }
  fd = dup(root_fd);
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to access skill root", strerror(errno));
  }
  cursor = id;
  for (;;) {
    slash = strchr(cursor, '/');
    length = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
    memcpy(component, cursor, length);
    component[length] = '\0';
    next_fd =
        openat(fd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    (void)close(fd);
    if (next_fd < 0) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "skill package is unavailable",
                                  strerror(errno));
    }
    fd = next_fd;
    if (slash == NULL) {
      break;
    }
    cursor = slash + 1;
  }
  cursor = resource != NULL ? resource : "SKILL.md";
  for (;;) {
    slash = strchr(cursor, '/');
    length = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
    memcpy(component, cursor, length);
    component[length] = '\0';
    if (slash == NULL) {
      next_fd = openat(fd, component, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    } else {
      next_fd = openat(fd, component,
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    (void)close(fd);
    if (next_fd < 0) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "skill resource is unavailable",
                                  strerror(errno));
    }
    fd = next_fd;
    if (slash == NULL) {
      break;
    }
    cursor = slash + 1;
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
    (void)close(fd);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill resource must be a private regular file");
  }
  *out_fd = fd;
  return CAI_OK;
}

static int cai_skill_local_read(void *context, const char *skill_id,
                                const char *resource, cai_source **out,
                                cai_error *error) {
  cai_local_skill_provider *local = (cai_local_skill_provider *)context;
  int fd;
  FILE *fp;
  int rc;

  if (local == NULL || local->root_fd < 0 || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "skill provider is closed");
  }
  *out = NULL;
  rc = cai_skill_open_relative_file(local->root_fd, skill_id, resource, &fd,
                                    error);
  if (rc != CAI_OK) {
    return rc;
  }
  fp = fdopen(fd, "rb");
  if (fp == NULL) {
    (void)close(fd);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open skill resource",
                                strerror(errno));
  }
  return cai_source_file(fp, 1, out, error);
}

static int cai_skill_catalog_grow(cai_skill_catalog *catalog,
                                  cai_error *error) {
  cai_skill_entry *entries;
  size_t capacity;

  if (catalog->count < catalog->capacity) {
    return CAI_OK;
  }
  capacity = catalog->capacity == 0U ? 8U : catalog->capacity * 2U;
  entries = (cai_skill_entry *)cai_realloc_mem(NULL, catalog->entries,
                                               capacity * sizeof(*entries));
  if (entries == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow skill catalog");
  }
  catalog->entries = entries;
  catalog->capacity = capacity;
  return CAI_OK;
}

void cai_skills_catalog_cleanup(cai_skill_catalog *catalog) {
  size_t i;

  if (catalog == NULL) {
    return;
  }
  for (i = 0U; i < catalog->count; i++) {
    cai_free_mem(NULL, catalog->entries[i].id);
    cai_free_mem(NULL, catalog->entries[i].name);
    cai_free_mem(NULL, catalog->entries[i].description);
  }
  cai_free_mem(NULL, catalog->entries);
  if (catalog->local != NULL) {
    cai_skill_local_cleanup(catalog->local);
  }
  cai_free_mem(NULL, catalog);
}

int cai_skills_catalog_has_entries(const cai_skill_catalog *catalog) {
  return catalog != NULL && catalog->count > 0U;
}

static int cai_skill_read_source(cai_source *source, char **out, size_t maximum,
                                 cai_error *error) {
  char chunk[4096];
  char *data;
  size_t length;
  size_t capacity;
  size_t nread;
  int rc;

  *out = NULL;
  data = NULL;
  length = 0U;
  capacity = 0U;
  rc = CAI_OK;
  while ((nread = cai_source_read(source, chunk, sizeof(chunk), error)) > 0U) {
    if (nread > maximum - length) {
      rc = cai_set_error(error, CAI_ERR_INVALID, "skill resource is too large");
      break;
    }
    if (length + nread + 1U > capacity) {
      size_t next = capacity == 0U ? 4096U : capacity;
      while (next < length + nread + 1U) {
        if (next > maximum / 2U) {
          next = maximum + 1U;
          break;
        }
        next *= 2U;
      }
      data = (char *)cai_realloc_mem(NULL, data, next);
      if (data == NULL) {
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to read skill resource");
        break;
      }
      capacity = next;
    }
    memcpy(data + length, chunk, nread);
    length += nread;
  }
  if (rc == CAI_OK && error != NULL && error->code != CAI_OK) {
    rc = error->code;
  }
  if (rc == CAI_OK && data == NULL) {
    data = cai_strdup(NULL, "");
    if (data == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM, "failed to read skill resource");
    }
  }
  if (rc == CAI_OK) {
    data[length] = '\0';
    *out = data;
  } else {
    cai_free_mem(NULL, data);
  }
  return rc;
}

static int cai_skill_parse_frontmatter(const char *text, const char *fallback,
                                       char **out_name, char **out_description,
                                       cai_error *error) {
  const char *cursor;
  const char *line_end;
  const char *value;
  size_t line_length;
  char *name;
  char *description;

  *out_name = NULL;
  *out_description = NULL;
  if (strncmp(text, "---\n", 4U) != 0 && strncmp(text, "---\r\n", 5U) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "SKILL.md must begin with YAML frontmatter");
  }
  cursor = strchr(text, '\n') + 1;
  name = NULL;
  description = NULL;
  while (*cursor != '\0') {
    line_end = strchr(cursor, '\n');
    line_length =
        line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);
    if (line_length > 0U && cursor[line_length - 1U] == '\r') {
      line_length--;
    }
    if (line_length == 3U && strncmp(cursor, "---", 3U) == 0) {
      break;
    }
    if (line_length > 5U && strncmp(cursor, "name:", 5U) == 0) {
      value = cursor + 5U;
      while (*value == ' ' || *value == '\t')
        value++;
      cai_free_mem(NULL, name);
      name = cai_strndup(NULL, value, line_length - (size_t)(value - cursor));
    } else if (line_length > 12U && strncmp(cursor, "description:", 12U) == 0) {
      value = cursor + 12U;
      while (*value == ' ' || *value == '\t')
        value++;
      cai_free_mem(NULL, description);
      description =
          cai_strndup(NULL, value, line_length - (size_t)(value - cursor));
    }
    if (line_end == NULL)
      break;
    cursor = line_end + 1U;
  }
  if (*cursor == '\0' || description == NULL || description[0] == '\0') {
    cai_free_mem(NULL, name);
    cai_free_mem(NULL, description);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "SKILL.md frontmatter requires description");
  }
  if (name == NULL || name[0] == '\0') {
    const char *basename;

    basename = strrchr(fallback, '/');
    basename = basename != NULL ? basename + 1U : fallback;
    cai_free_mem(NULL, name);
    name = cai_strdup(NULL, basename);
  }
  if (name == NULL || !cai_skill_segment_valid(name) || strlen(name) > 64U ||
      strlen(description) > 1024U || strpbrk(description, "\r\n") != NULL) {
    cai_free_mem(NULL, name);
    cai_free_mem(NULL, description);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "SKILL.md frontmatter is invalid");
  }
  *out_name = name;
  *out_description = description;
  return CAI_OK;
}

static int cai_skill_catalog_visit(void *context, const char *skill_id,
                                   cai_error *error) {
  cai_skill_catalog *catalog = (cai_skill_catalog *)context;
  cai_source *source;
  char *text;
  char *name;
  char *description;
  size_t i;
  int rc;

  (void)error;
  if (!cai_skill_id_valid(skill_id) ||
      catalog->count >= CAI_SKILLS_MAX_PACKAGES) {
    cai_skills_warn(catalog, "skipping invalid or excess configured skill");
    return CAI_OK;
  }
  source = NULL;
  text = NULL;
  name = NULL;
  description = NULL;
  rc = catalog->provider.read(catalog->provider.context, skill_id, NULL,
                              &source, error);
  if (rc == CAI_OK && source == NULL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "skill provider returned no SKILL.md source");
  }
  if (rc == CAI_OK) {
    rc = cai_skill_read_source(source, &text, CAI_SKILLS_MAX_FILE_BYTES, error);
  }
  if (source != NULL)
    cai_source_close(source);
  if (rc == CAI_OK) {
    rc =
        cai_skill_parse_frontmatter(text, skill_id, &name, &description, error);
  }
  cai_free_mem(NULL, text);
  if (rc != CAI_OK) {
    cai_error warning;
    const char *detail;
    cai_error_init(&warning);
    detail = error != NULL && error->message != NULL ? error->message
                                                     : "invalid SKILL.md";
    cai_skills_warn(catalog, detail);
    cai_error_cleanup(&warning);
    cai_error_cleanup(error);
    return CAI_OK;
  }
  for (i = 0U; i < catalog->count; i++) {
    if (strcmp(catalog->entries[i].name, name) == 0) {
      cai_free_mem(NULL, name);
      cai_free_mem(NULL, description);
      cai_skills_warn(catalog, "skipping duplicate configured skill name");
      return CAI_OK;
    }
  }
  rc = cai_skill_catalog_grow(catalog, error);
  if (rc == CAI_OK) {
    catalog->entries[catalog->count].id = cai_strdup(NULL, skill_id);
    catalog->entries[catalog->count].name = name;
    catalog->entries[catalog->count].description = description;
    if (catalog->entries[catalog->count].id == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM, "failed to copy skill id");
    } else {
      catalog->count++;
      return CAI_OK;
    }
  }
  cai_free_mem(NULL, name);
  cai_free_mem(NULL, description);
  return rc;
}

static int cai_skill_local_scan_dir(cai_local_skill_provider *local, int dir_fd,
                                    const char *prefix, size_t depth,
                                    cai_skill_provider_visit_fn visit,
                                    void *visit_context, cai_error *error) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  char path[1024];
  int rc;

  (void)local;
  dir = fdopendir(dup(dir_fd));
  if (dir == NULL)
    return CAI_OK;
  rc = CAI_OK;
  while (rc == CAI_OK && (entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.' || !cai_skill_segment_valid(entry->d_name))
      continue;
    if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(st.st_mode))
      continue;
    if (prefix[0] == '\0')
      snprintf(path, sizeof(path), "%s", entry->d_name);
    else
      snprintf(path, sizeof(path), "%s/%s", prefix, entry->d_name);
    if (strlen(path) >= sizeof(path) - 1U)
      continue;
    if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
      int skill_fd = openat(dir_fd, entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (skill_fd >= 0) {
        struct stat skill_st;
        if (fstatat(skill_fd, "SKILL.md", &skill_st, AT_SYMLINK_NOFOLLOW) ==
                0 &&
            S_ISREG(skill_st.st_mode) && skill_st.st_nlink == 1) {
          rc = visit(visit_context, path, error);
        }
        if (rc == CAI_OK && depth < CAI_SKILLS_MAX_DEPTH) {
          rc = cai_skill_local_scan_dir(local, skill_fd, path, depth + 1U,
                                        visit, visit_context, error);
        }
        (void)close(skill_fd);
      }
    }
  }
  closedir(dir);
  return rc;
}

static int cai_skill_local_list(void *context,
                                cai_skill_provider_visit_fn visit,
                                void *visit_context, cai_error *error) {
  cai_local_skill_provider *local = (cai_local_skill_provider *)context;
  if (local == NULL || local->root_fd < 0 || visit == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "skill provider is invalid");
  }
  return cai_skill_local_scan_dir(local, local->root_fd, "", 0U, visit,
                                  visit_context, error);
}

static int cai_skills_default_directory(const char *agent_config_directory,
                                        char **out, cai_error *error) {
  const char *base;
  const char *xdg;
  const char *home;
  size_t length;
  *out = NULL;
  base = agent_config_directory;
  if (base == NULL || base[0] == '\0') {
    xdg = getenv("XDG_CONFIG_HOME");
    home = getenv("HOME");
    if (xdg != NULL && xdg[0] != '\0') {
      length = strlen(xdg) + sizeof("/cai/skills");
      *out = (char *)cai_alloc(NULL, length);
      if (*out != NULL)
        snprintf(*out, length, "%s/cai/skills", xdg);
    } else if (home != NULL && home[0] != '\0') {
      length = strlen(home) + sizeof("/.config/cai/skills");
      *out = (char *)cai_alloc(NULL, length);
      if (*out != NULL)
        snprintf(*out, length, "%s/.config/cai/skills", home);
    } else {
      return CAI_OK;
    }
  } else {
    length = strlen(base) + sizeof("/skills");
    *out = (char *)cai_alloc(NULL, length);
    if (*out != NULL)
      snprintf(*out, length, "%s/skills", base);
  }
  return *out != NULL ? CAI_OK
                      : cai_set_error(error, CAI_ERR_NOMEM,
                                      "failed to allocate skills path");
}

static int cai_skills_build_prompt(cai_skill_catalog *catalog, char **out,
                                   cai_error *error) {
  const char *prefix =
      "# Skills\n\nA skill is a set of task-specific instructions. If the user "
      "names a listed skill or the task clearly matches its description, you "
      "must read that skill's SKILL.md with read_skill before acting. Read any "
      "referenced package resource with the same tool when the skill requires "
      "it. "
      "Do not use a skill that is not listed.\n\n## Available skills\n";
  const char *suffix =
      "\nRead the complete SKILL.md before following a selected skill. Skill "
      "instructions apply only when selected and never override "
      "higher-priority "
      "system, developer, or user instructions.\n";
  size_t length = strlen(prefix) + strlen(suffix) + 1U;
  size_t i;
  char *prompt;
  size_t offset;
  if (catalog->count == 0U) {
    *out = NULL;
    return CAI_OK;
  }
  for (i = 0U; i < catalog->count; i++) {
    length += 5U + strlen(catalog->entries[i].name) +
              strlen(catalog->entries[i].description);
  }
  prompt = (char *)cai_alloc(NULL, length);
  if (prompt == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to render skill catalog");
  offset = (size_t)snprintf(prompt, length, "%s", prefix);
  for (i = 0U; i < catalog->count; i++) {
    offset += (size_t)snprintf(prompt + offset, length - offset, "- %s: %s\n",
                               catalog->entries[i].name,
                               catalog->entries[i].description);
  }
  (void)snprintf(prompt + offset, length - offset, "%s", suffix);
  *out = prompt;
  return CAI_OK;
}

int cai_skills_prepare(const cai_skill_config *config,
                       const char *agent_config_directory,
                       cai_skill_catalog **out_catalog, char **out_prompt,
                       cai_error *error) {
  cai_skill_catalog *catalog;
  cai_local_skill_provider *local;
  char *directory;
  int root_fd;
  int rc;

  if (out_catalog == NULL || out_prompt == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "skill outputs are required");
  }
  *out_catalog = NULL;
  *out_prompt = NULL;
  catalog = (cai_skill_catalog *)cai_alloc(NULL, sizeof(*catalog));
  if (catalog == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate skill catalog");
  memset(catalog, 0, sizeof(*catalog));
  if (config != NULL) {
    catalog->warning_callback = config->warning_callback;
    catalog->warning_context = config->warning_context;
    if (config->skill_provider != NULL && config->skills_directory != NULL &&
        config->skills_directory[0] != '\0') {
      cai_skills_catalog_cleanup(catalog);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "skill provider and skills directory are exclusive");
    }
  }
  if (config != NULL && config->skill_provider != NULL) {
    if (config->skill_provider->list == NULL ||
        config->skill_provider->read == NULL) {
      cai_skills_catalog_cleanup(catalog);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "skill provider requires list and read callbacks");
    }
    catalog->provider = *config->skill_provider;
  } else {
    directory = NULL;
    rc = CAI_OK;
    if (config != NULL && config->skills_directory != NULL &&
        config->skills_directory[0] != '\0')
      directory = cai_strdup(NULL, config->skills_directory);
    else
      rc = cai_skills_default_directory(agent_config_directory, &directory,
                                        error);
    if (directory == NULL &&
        (config == NULL || config->skills_directory == NULL ||
         config->skills_directory[0] == '\0'))
      rc = CAI_OK;
    if (directory == NULL && config != NULL &&
        config->skills_directory != NULL &&
        config->skills_directory[0] != '\0') {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate configured skills directory");
    }
    if (directory == NULL && rc != CAI_OK) {
      cai_skills_catalog_cleanup(catalog);
      return rc;
    }
    root_fd =
        directory != NULL
            ? open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
            : -1;
    if (root_fd < 0) {
      if (directory != NULL && errno != ENOENT)
        cai_skills_warn(catalog, "configured skills directory is unavailable");
      cai_free_mem(NULL, directory);
      *out_catalog = catalog;
      return CAI_OK;
    }
    cai_free_mem(NULL, directory);
    local = (cai_local_skill_provider *)cai_alloc(NULL, sizeof(*local));
    if (local == NULL) {
      (void)close(root_fd);
      cai_skills_catalog_cleanup(catalog);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate local skill provider");
    }
    local->root_fd = root_fd;
    catalog->local = local;
    catalog->provider.list = cai_skill_local_list;
    catalog->provider.read = cai_skill_local_read;
    catalog->provider.context = local;
  }
  rc = catalog->provider.list(catalog->provider.context,
                              cai_skill_catalog_visit, catalog, error);
  if (rc != CAI_OK) {
    cai_skills_warn(catalog, "configured skill discovery failed; continuing "
                             "without unavailable skills");
    cai_error_cleanup(error);
  }
  rc = cai_skills_build_prompt(catalog, out_prompt, error);
  if (rc != CAI_OK) {
    cai_skills_catalog_cleanup(catalog);
    return rc;
  }
  *out_catalog = catalog;
  return CAI_OK;
}

static int cai_skill_find(cai_skill_catalog *catalog, const char *name,
                          const char **out_id) {
  size_t i;
  for (i = 0U; i < catalog->count; i++) {
    if (strcmp(catalog->entries[i].name, name) == 0) {
      *out_id = catalog->entries[i].id;
      return 1;
    }
  }
  return 0;
}

static int cai_skill_append(lonejson_spooled *spool, const char *data,
                            size_t len, cai_error *error) {
  lonejson_error json_error;
  lonejson_error_init(&json_error);
  if (spool->append(spool, data, len, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool skill resource",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_skill_callback(void *context, const void *params, void *result,
                              cai_error *error) {
  cai_skill_catalog *catalog = (cai_skill_catalog *)context;
  const cai_skill_args *args = (const cai_skill_args *)params;
  cai_skill_result *out = (cai_skill_result *)result;
  const char *id;
  const char *resource;
  cai_source *source;
  char buffer[4096];
  size_t nread;
  size_t total;
  int rc;
  if (catalog == NULL || args == NULL || out == NULL ||
      !cai_skill_find(catalog, args->skill, &id)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "requested skill is not available in this agent");
  }
  resource = args->resource != NULL && args->resource[0] != '\0'
                 ? args->resource
                 : "SKILL.md";
  if (args->resource != NULL &&
      !cai_skill_relative_path_valid(args->resource)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill resource path is invalid");
  }
  source = NULL;
  rc = catalog->provider.read(
      catalog->provider.context, id,
      strcmp(resource, "SKILL.md") == 0 ? NULL : resource, &source, error);
  if (rc != CAI_OK)
    return rc;
  if (source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill provider returned no resource source");
  }
  CAI_LJ->spooled_init(CAI_LJ, &out->content);
  total = 0U;
  while ((nread = cai_source_read(source, buffer, sizeof(buffer), error)) >
         0U) {
    if (nread > CAI_SKILLS_MAX_FILE_BYTES - total) {
      rc = cai_set_error(error, CAI_ERR_INVALID, "skill resource is too large");
      break;
    }
    rc = cai_skill_append(&out->content, buffer, nread, error);
    if (rc != CAI_OK)
      break;
    total += nread;
  }
  if (rc == CAI_OK && error != NULL && error->code != CAI_OK) {
    rc = error->code;
  }
  cai_source_close(source);
  if (rc != CAI_OK) {
    out->content.cleanup(&out->content);
    return rc;
  }
  out->skill = cai_strdup(NULL, args->skill);
  out->resource = cai_strdup(NULL, resource);
  if (out->skill == NULL || out->resource == NULL) {
    cai_free_mem(NULL, out->skill);
    cai_free_mem(NULL, out->resource);
    out->content.cleanup(&out->content);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate skill result");
  }
  return cai_tool_result_set_spooled(&cai_skill_result_map, out, "content",
                                     &out->content, error);
}

int cai_agent_register_skill_tool_owned(cai_agent *agent,
                                        cai_skill_catalog *catalog,
                                        cai_error *error) {
  cai_agent_impl *impl;
  int rc;
  if (agent == NULL || agent->impl == NULL || catalog == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent and skill catalog are required");
  }
  impl = CAI_AGENT_IMPL(agent);
  rc = cai_tool_registry_register_lonejson_schema_owned(
      impl->tools, CAI_SKILL_READ_TOOL_NAME,
      "Reads the complete SKILL.md or a package-relative resource for one "
      "listed global skill. "
      "Use only a skill named in the developer-provided catalog.",
      cai_skill_schema_json, 0, &cai_skill_args_map, &cai_skill_result_map,
      cai_skill_callback, catalog, (void (*)(void *))cai_skills_catalog_cleanup,
      error);
  return rc;
}
