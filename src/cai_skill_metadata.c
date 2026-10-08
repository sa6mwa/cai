#include "cai_internal.h"
#include <cai/skills.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#define SKILL_HEADER_MAX (1024U * 1024U)
typedef struct skill_metadata_impl {
  char *name;
  char *description;
} skill_metadata_impl;
const char *cai_skill_metadata_name(const cai_skill_metadata *self) {
  return self != NULL ? ((skill_metadata_impl *)self->impl)->name : NULL;
}
const char *cai_skill_metadata_description(const cai_skill_metadata *self) {
  return self != NULL ? ((skill_metadata_impl *)self->impl)->description : NULL;
}
void cai_skill_metadata_close(cai_skill_metadata *self) {
  skill_metadata_impl *impl;
  if (self == NULL)
    return;
  impl = self->impl;
  if (impl != NULL) {
    cai_free_mem(NULL, impl->name);
    cai_free_mem(NULL, impl->description);
    cai_free_mem(NULL, impl);
  }
  cai_free_mem(NULL, self);
}
static int marker(const char *line, size_t length) {
  if (length > 0U && line[length - 1U] == '\r')
    length--;
  if (length < 3U || memcmp(line, "---", 3U) != 0)
    return 0;
  line += 3U;
  length -= 3U;
  while (length && (*line == ' ' || *line == '\t')) {
    line++;
    length--;
  }
  return length == 0U || *line == '#';
}
static int read_header(cai_source *source, cai_buffer_builder *header,
                       cai_error *error) {
  char bytes[4096];
  size_t n, i, start = 0U;
  int opening = 0, closing = 0, rc = CAI_OK;
  while (!closing && rc == CAI_OK &&
         (n = source->read(source, bytes, sizeof(bytes), error)) != 0U) {
    for (i = 0U; i < n && rc == CAI_OK && !closing; i++) {
      if (bytes[i] == '\0') {
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "NUL byte in skill front matter");
        break;
      }
      if (bytes[i] == '\n') {
        const char *line = header->data != NULL ? header->data + start : "";
        size_t length = header->length - start;
        if (!opening && length >= 3U && memcmp(line, "\xef\xbb\xbf", 3U) == 0) {
          line += 3U;
          length -= 3U;
        }
        if (!opening) {
          if (!marker(line, length)) {
            rc = cai_set_error(error, CAI_ERR_INVALID,
                               "SKILL.md must begin with YAML front matter");
            break;
          }
          opening = 1;
        } else if (marker(line, length)) {
          header->length = start;
          header->data[start] = '\0';
          closing = 1;
          break;
        }
        if (header->length >= SKILL_HEADER_MAX)
          rc = cai_set_error(error, CAI_ERR_INVALID,
                             "skill front matter exceeds 1 MiB");
        else
          rc = cai_buffer_append(header, "\n", 1U, error);
        start = header->length;
      } else if (header->length >= SKILL_HEADER_MAX)
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "skill front matter exceeds 1 MiB");
      else
        rc = cai_buffer_append(header, bytes + i, 1U, error);
    }
  }
  if (rc == CAI_OK && error->code != CAI_OK)
    rc = error->code;
  if (rc == CAI_OK && opening && !closing && header->data != NULL &&
      marker(header->data + start, header->length - start)) {
    header->length = start;
    header->data[start] = '\0';
    closing = 1;
  }
  if (rc == CAI_OK && !closing)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "SKILL.md front matter has no closing --- delimiter");
  return rc;
}
static int scalar(yaml_node_t *node, size_t maximum, char **out,
                  cai_error *error) {
  if (node == NULL || node->type != YAML_SCALAR_NODE ||
      node->data.scalar.length > maximum ||
      strcmp((const char *)node->tag, YAML_STR_TAG) != 0 ||
      memchr(node->data.scalar.value, '\0', node->data.scalar.length) != NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill metadata must contain bounded string scalars");
  *out = cai_strndup(NULL, (const char *)node->data.scalar.value,
                     node->data.scalar.length);
  return *out != NULL
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_NOMEM, "copy skill metadata");
}
static int parse_header(cai_buffer_builder *header, const char *fallback,
                        skill_metadata_impl *out, cai_error *error) {
  yaml_parser_t parser;
  yaml_document_t document;
  yaml_node_t *root, *key, *value;
  yaml_node_pair_t *pair;
  char *read, *write;
  const unsigned char *name;
  const char *base;
  int rc = CAI_OK, saw_name = 0, saw_description = 0, space = 0;
  char message[256];
  if (!yaml_parser_initialize(&parser))
    return cai_set_error(error, CAI_ERR_NOMEM, "initialize skill YAML parser");
  yaml_parser_set_input_string(&parser, (const unsigned char *)header->data,
                               header->length);
  if (!yaml_parser_load(&parser, &document)) {
    snprintf(message, sizeof(message),
             "invalid skill YAML at line %lu, column %lu: %s",
             (unsigned long)parser.problem_mark.line + 1UL,
             (unsigned long)parser.problem_mark.column + 1UL,
             parser.problem != NULL ? parser.problem : "parse failed");
    yaml_parser_delete(&parser);
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  root = yaml_document_get_root_node(&document);
  if (root == NULL || root->type != YAML_MAPPING_NODE)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "skill front matter must be a YAML mapping");
  else
    for (pair = root->data.mapping.pairs.start;
         pair < root->data.mapping.pairs.top && rc == CAI_OK; pair++) {
      key = yaml_document_get_node(&document, pair->key);
      value = yaml_document_get_node(&document, pair->value);
      if (key == NULL || key->type != YAML_SCALAR_NODE)
        continue;
      if (key->data.scalar.length == 4U &&
          memcmp(key->data.scalar.value, "name", 4U) == 0) {
        if (saw_name++)
          rc = cai_set_error(error, CAI_ERR_INVALID,
                             "duplicate skill name field");
        else
          rc = scalar(value, 64U, &out->name, error);
      } else if (key->data.scalar.length == 11U &&
                 memcmp(key->data.scalar.value, "description", 11U) == 0) {
        if (saw_description++)
          rc = cai_set_error(error, CAI_ERR_INVALID,
                             "duplicate skill description field");
        else
          rc = scalar(value, 1024U, &out->description, error);
      }
    }
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  if (rc != CAI_OK)
    return rc;
  if (out->name == NULL || out->name[0] == '\0') {
    base = strrchr(fallback, '/');
    base = base != NULL ? base + 1U : fallback;
    cai_free_mem(NULL, out->name);
    out->name = cai_strdup(NULL, base);
    if (out->name == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "copy fallback skill name");
  }
  if (out->name[0] == '\0' || strlen(out->name) > 64U ||
      strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0)
    return cai_set_error(error, CAI_ERR_INVALID, "invalid skill name");
  for (name = (const unsigned char *)out->name; *name; name++)
    if (*name < 0x21U || *name == 0x7fU || *name == '/' || *name == '\\')
      return cai_set_error(error, CAI_ERR_INVALID, "invalid skill name");
  if (out->description == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill front matter requires description");
  write = out->description;
  for (read = out->description; *read; read++) {
    if (isspace((unsigned char)*read)) {
      space = write != out->description;
      continue;
    }
    if (space)
      *write++ = ' ';
    *write++ = *read;
    space = 0;
  }
  *write = '\0';
  return out->description[0] != '\0'
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_INVALID,
                             "skill description is empty");
}
int cai_skill_metadata_open(cai_source *source, const char *fallback_name,
                            cai_skill_metadata **out, cai_error *error) {
  cai_buffer_builder header;
  cai_skill_metadata *self;
  skill_metadata_impl *impl;
  cai_error local;
  int rc;
  if (error == NULL) {
    cai_error_init(&local);
    rc = cai_skill_metadata_open(source, fallback_name, out, &local);
    cai_error_cleanup(&local);
    return rc;
  }
  if (out == NULL || source == NULL || fallback_name == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "skill metadata source, name and output are required");
  *out = NULL;
  self = cai_alloc(NULL, sizeof(*self));
  impl = cai_alloc(NULL, sizeof(*impl));
  if (self == NULL || impl == NULL) {
    cai_free_mem(NULL, self);
    cai_free_mem(NULL, impl);
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate skill metadata");
  }
  memset(impl, 0, sizeof(*impl));
  self->impl = impl;
  self->name = cai_skill_metadata_name;
  self->description = cai_skill_metadata_description;
  self->close = cai_skill_metadata_close;
  memset(&header, 0, sizeof(header));
  rc = read_header(source, &header, error);
  if (rc == CAI_OK)
    rc = parse_header(&header, fallback_name, impl, error);
  cai_free_mem(NULL, header.data);
  if (rc != CAI_OK) {
    self->close(self);
    return rc;
  }
  *out = self;
  return CAI_OK;
}
