#include "message_names.h"
#include "pouch_stream.h"
#include <stdlib.h>
#include <string.h>

typedef struct name_path {
  char *storage;
  const char **parts;
  size_t count;
} name_path;

typedef struct name_search {
  const cai_cli_message_name *self, *known;
  size_t count;
  name_path own, *others;
  size_t other_count, *eligible, eligible_count, *chosen;
  char *out;
} name_search;

static const char *basename_of(const char *path) {
  const char *p = strrchr(path, '/');
  return p != NULL ? p + 1 : path;
}

static int same_workspace(const cai_cli_message_name *a,
                          const cai_cli_message_name *b) {
  return a->host_id[0] != '\0' && strcmp(a->host_id, b->host_id) == 0 &&
         strcmp(a->workspace, b->workspace) == 0;
}

static int component_valid(const char *p) {
  return p[0] != '\0' &&
         strspn(p, "abcdefghijklmnopqrstuvwxyz0123456789._-") == strlen(p);
}

static void path_free(name_path *p) {
  free(p->parts);
  free(p->storage);
}

static int path_split(const char *hostname, const char *workspace,
                      name_path *out) {
  size_t capacity = 1U, n = strlen(hostname), length = strlen(workspace);
  const char *p;
  char *cursor;
  memset(out, 0, sizeof(*out));
  for (p = workspace; *p != '\0'; p++)
    if (*p == '/')
      capacity++;
  out->storage = malloc(n + length + 1U);
  out->parts = malloc(capacity * sizeof(*out->parts));
  if (out->storage == NULL || out->parts == NULL) {
    path_free(out);
    memset(out, 0, sizeof(*out));
    return -1;
  }
  memcpy(out->storage, hostname, n);
  memcpy(out->storage + n, workspace, length + 1U);
  cursor = out->storage;
  out->parts[out->count++] = cursor;
  while (*cursor != '\0') {
    if (*cursor == '/') {
      *cursor++ = '\0';
      if (*cursor != '\0')
        out->parts[out->count++] = cursor;
    } else
      cursor++;
  }
  return 0;
}

static int name_occupied(const name_search *s, const char *name) {
  size_t i;
  for (i = 0U; i < s->count; i++)
    if (strcmp(name, s->known[i].name) == 0 &&
        !same_workspace(s->self, &s->known[i]))
      return 1;
  return 0;
}

/* Selection is stored nearest-first; matching and output follow path order. */
static int selection_present(const name_search *s, const name_path *other,
                             size_t chosen) {
  size_t i = 0U, at;
  while (chosen > 0U) {
    at = s->chosen[--chosen];
    while (i + 1U < other->count &&
           strcmp(s->own.parts[at], other->parts[i]) != 0)
      i++;
    if (i + 1U >= other->count)
      return 0;
    i++;
  }
  return 1;
}

static int candidate(name_search *s, size_t chosen) {
  char name[129];
  size_t i, used = 4U, n;
  for (i = 0U; i < s->other_count; i++)
    if (selection_present(s, &s->others[i], chosen))
      return 0;
  memcpy(name, "cai.", 4U);
  while (chosen > 0U) {
    const char *part = s->own.parts[s->chosen[--chosen]];
    n = strlen(part);
    memcpy(name + used, part, n);
    used += n;
    name[used++] = '.';
  }
  strcpy(name + used, s->own.parts[s->own.count - 1U]);
  if (name_occupied(s, name))
    return 0;
  strcpy(s->out, name);
  return 1;
}

static int choose(name_search *s, size_t next, size_t selected, size_t left,
                  size_t bytes) {
  size_t i, j, at, n;
  if (left == 0U)
    return candidate(s, selected);
  for (i = next; i >= left && i > 0U; i--) {
    at = s->eligible[i - 1U];
    /* The nearest occurrence of an identical component admits every prefix
     * available to earlier occurrences. Explore that representative once. */
    for (j = i; j < next; j++)
      if (strcmp(s->own.parts[at], s->own.parts[s->eligible[j]]) == 0)
        break;
    if (j != next)
      continue;
    n = strlen(s->own.parts[at]) + 1U;
    if (n > 128U - bytes)
      continue;
    s->chosen[selected] = at;
    if (choose(s, i - 1U, selected + 1U, left - 1U, bytes + n))
      return 1;
  }
  return 0;
}

int cai_cli_message_name_select(const cai_cli_message_name *self,
                                const cai_cli_message_name *known, size_t count,
                                char out[129], cai_error *error) {
  name_search s;
  const char *base = basename_of(self->workspace);
  size_t i, want, bytes = 4U + strlen(base);
  int rc = CAI_ERR_INVALID;
  memset(&s, 0, sizeof(s));
  s.self = self;
  s.known = known;
  s.count = count;
  s.out = out;
  for (i = 0U; i < count; i++)
    if (known[i].automatic && same_workspace(self, &known[i])) {
      if (!component_valid(known[i].name) || strlen(known[i].name) > 128U)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "stored automatic mailbox name is invalid");
      strcpy(out, known[i].name);
      return CAI_OK;
    }
  for (i = 0U; i < count; i++) {
    if (known[i].host_id[0] != '\0' &&
        strcmp(known[i].host_id, self->host_id) != 0 &&
        strcmp(known[i].hostname, self->hostname) == 0 &&
        strcmp(known[i].workspace, self->workspace) == 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Different hosts have the same hostname and "
                           "workspace path; use --message-queue NAME.");
  }
  if (!component_valid(base) || bytes > 128U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "The workspace basename cannot be represented "
        "unchanged as a queue name; use --message-queue NAME.");
  strcpy(out, "cai.");
  strcat(out, base);
  if (!name_occupied(&s, out))
    return CAI_OK;
  if (path_split(self->hostname, self->workspace, &s.own) != 0)
    goto nomem;
  s.others = calloc(count == 0U ? 1U : count, sizeof(*s.others));
  s.eligible = malloc(s.own.count * sizeof(*s.eligible));
  s.chosen = malloc(s.own.count * sizeof(*s.chosen));
  if (s.others == NULL || s.eligible == NULL || s.chosen == NULL)
    goto nomem;
  for (i = 0U; i < count; i++) {
    if (same_workspace(self, &known[i]) || known[i].hostname[0] == '\0' ||
        strcmp(base, basename_of(known[i].workspace)) != 0)
      continue;
    if (path_split(known[i].hostname, known[i].workspace,
                   &s.others[s.other_count]) != 0)
      goto nomem;
    s.other_count++;
  }
  for (i = 0U; i + 1U < s.own.count; i++)
    if (component_valid(s.own.parts[i]) &&
        strlen(s.own.parts[i]) + 1U <= 128U - bytes)
      s.eligible[s.eligible_count++] = i;
  /* If even every usable ancestor occurs in a competing path, no subset
   * can distinguish it. This also avoids exponential searches of deep,
   * identical paths and of paths contained entirely in another workspace. */
  for (i = 0U; i < s.eligible_count; i++)
    s.chosen[i] = s.eligible[s.eligible_count - i - 1U];
  for (i = 0U; i < s.other_count; i++)
    if (selection_present(&s, &s.others[i], s.eligible_count))
      goto unavailable;
  for (want = 1U; want <= s.eligible_count && want <= (128U - bytes) / 2U;
       want++)
    if (choose(&s, s.eligible_count, 0U, want, bytes)) {
      rc = CAI_OK;
      goto done;
    }
unavailable:
  rc = cai_cli_error(error, CAI_ERR_INVALID,
                     "No available queue name can distinguish this workspace "
                     "using its actual hostname and directory components; "
                     "use --message-queue NAME.");
  goto done;
nomem:
  rc =
      cai_cli_error(error, CAI_ERR_NOMEM, "allocate workspace naming snapshot");
done:
  for (i = 0U; i < s.other_count; i++)
    path_free(&s.others[i]);
  free(s.others);
  free(s.eligible);
  free(s.chosen);
  path_free(&s.own);
  return rc;
}
