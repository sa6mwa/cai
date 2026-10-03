#include "cai_internal.h"
#include <cai/display.h>

#define CAI_DISPLAY_LINE_MAX 65536U

typedef struct cai_display_call {
  char *id;
  char *session;
  char *command;
  int mode;
  int command_active;
  size_t lines;
  int partial_line;
  int pending_cr;
  int omitted;
  char *line;
  size_t length;
  char *first;
  char *last;
  size_t first_length;
  size_t last_length;
  size_t context_lines;
  int changed;
  int long_line;
  int clipped;
  struct cai_display_call *next;
} cai_display_call;

struct cai_tool_display {
  cai_tool_display_config config;
  cai_display_call *calls;
  unsigned char utf8[4];
  size_t utf8_length;
  size_t utf8_expected;
};

typedef struct display_mcp_result {
  int is_error;
} display_mcp_result;
static const lonejson_field display_mcp_result_fields[] = {
    LONEJSON_FIELD_BOOL(display_mcp_result, is_error, "isError")};
LONEJSON_MAP_DEFINE(display_mcp_result_map, display_mcp_result,
                    display_mcp_result_fields);
typedef struct display_result_reader {
  const cai_agent_runtime_event *event;
  size_t offset;
} display_result_reader;
static lonejson_read_result
display_result_read(void *context, unsigned char *bytes, size_t capacity) {
  display_result_reader *reader = (display_result_reader *)context;
  lonejson_read_result result = lonejson_default_read_result();
  size_t n = reader->event->data_length - reader->offset;
  if (n > capacity)
    n = capacity;
  if (n != 0U)
    memcpy(bytes, reader->event->data + reader->offset, n);
  reader->offset += n;
  result.bytes_read = n;
  result.eof = reader->offset == reader->event->data_length;
  return result;
}
/* MCP tool failure is independent of successful HTTP/JSON-RPC transport.
 * Select only the top-level flag, skipping content without copying it. */
static int display_mcp_failed(const cai_agent_runtime_event *event) {
  display_mcp_result doc;
  display_result_reader reader;
  lonejson_error error;
  lonejson_status status;
  int failed;
  if (event->data_length == 0U)
    return 0;
  memset(&doc, 0, sizeof(doc));
  reader.event = event;
  reader.offset = 0U;
  lonejson_error_init(&error);
  status = CAI_LJ->parse_reader(CAI_LJ, &display_mcp_result_map, &doc,
                                display_result_read, &reader, &error);
  failed = status == LONEJSON_STATUS_OK && doc.is_error;
  CAI_LJ->cleanup(CAI_LJ, &display_mcp_result_map, &doc);
  return failed;
}

static int display_write(cai_tool_display *d, const char *s, size_t n,
                         cai_error *error) {
  if (n == 0U)
    return CAI_OK;
  return cai_sink_write(d->config.sink, s, n, error);
}
static int display_escape(cai_tool_display *d, unsigned char ch,
                          cai_error *error) {
  char escape[5];
  snprintf(escape, sizeof(escape), "\\x%02x", (unsigned)ch);
  return display_write(d, escape, 4U, error);
}
static int display_flush_utf8(cai_tool_display *d, cai_error *error) {
  size_t i;
  int rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < d->utf8_length; i++)
    rc = display_escape(d, d->utf8[i], error);
  d->utf8_length = 0U;
  return rc;
}
static int display_text(cai_tool_display *d, const char *s, cai_error *error) {
  int rc = display_flush_utf8(d, error);
  return rc == CAI_OK ? display_write(d, s, strlen(s), error) : rc;
}
/* Decode across payload chunks. Tool bytes must never become terminal controls
 * or malformed UTF-8, including when a command ends in a partial code point. */
static int display_safe(cai_tool_display *d, const char *s, size_t n,
                        cai_error *error) {
  size_t i, start;
  int rc;
  start = 0U;
  if (n == 0U)
    return CAI_OK;
  for (i = 0U; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (d->utf8_length == 0U && ch < 127U &&
        (ch >= 32U || ch == '\n' || ch == '\t'))
      continue;
    rc = display_write(d, s + start, i - start, error);
    if (rc != CAI_OK)
      return rc;
    start = i + 1U;
    if (d->utf8_length != 0U && (ch & 0xc0U) == 0x80U) {
      int valid;
      d->utf8[d->utf8_length++] = ch;
      if (d->utf8_length < d->utf8_expected)
        continue;
      valid = !(d->utf8[0] == 0xc2U && d->utf8[1] < 0xa0U) &&
              !(d->utf8[0] == 0xe0U && d->utf8[1] < 0xa0U) &&
              !(d->utf8[0] == 0xedU && d->utf8[1] >= 0xa0U) &&
              !(d->utf8[0] == 0xf0U && d->utf8[1] < 0x90U) &&
              !(d->utf8[0] == 0xf4U && d->utf8[1] >= 0x90U);
      if (valid) {
        rc = display_write(d, (const char *)d->utf8, d->utf8_length, error);
        d->utf8_length = 0U;
      } else
        rc = display_flush_utf8(d, error);
      if (rc != CAI_OK)
        return rc;
      continue;
    }
    rc = display_flush_utf8(d, error);
    if (rc != CAI_OK)
      return rc;
    if (ch >= 0xc2U && ch <= 0xf4U) {
      d->utf8[0] = ch;
      d->utf8_length = 1U;
      d->utf8_expected = ch < 0xe0U ? 2U : ch < 0xf0U ? 3U : 4U;
    } else if (ch < 127U && (ch >= 32U || ch == '\n' || ch == '\t')) {
      rc = display_write(d, (const char *)&s[i], 1U, error);
      if (rc != CAI_OK)
        return rc;
    } else {
      rc = display_escape(d, ch, error);
      if (rc != CAI_OK)
        return rc;
    }
  }
  return display_write(d, s + start, n - start, error);
}
/* Keep a split code point for the next bounded line chunk, so an inserted SGR
 * reset never interrupts a valid UTF-8 sequence. */
static size_t patch_flush_length(const char *s, size_t n) {
  size_t start = n - 1U, expected;
  unsigned char lead;
  while (start > 0U && n - start < 4U &&
         ((unsigned char)s[start] & 0xc0U) == 0x80U)
    start--;
  lead = (unsigned char)s[start];
  expected = lead >= 0xc2U && lead < 0xe0U    ? 2U
             : lead >= 0xe0U && lead < 0xf0U  ? 3U
             : lead >= 0xf0U && lead <= 0xf4U ? 4U
                                              : 1U;
  return start + expected > n ? start : n;
}
static int display_marker(cai_tool_display *d, int result, const char *verb,
                          const char *text, size_t n, cai_error *error) {
  int rc;
  const char *star = !d->config.color || result < 0 ? "* "
                     : result                       ? "\033[92m*\033[0m "
                                                    : "\033[91m*\033[0m ";
  rc = display_text(d, star, error);
  if (rc == CAI_OK && d->config.color)
    rc = display_text(d, "\033[1;96m", error);
  if (rc == CAI_OK)
    rc = display_text(d, verb, error);
  if (rc == CAI_OK && n != 0U)
    rc = display_text(d, " ", error);
  if (rc == CAI_OK)
    rc = display_safe(d, text, n, error);
  if (rc == CAI_OK && d->config.color)
    rc = display_text(d, "\033[0m", error);
  if (rc == CAI_OK)
    rc = display_text(d, "\n", error);
  return rc;
}
static void display_call_free(cai_display_call *c) {
  cai_free_mem(NULL, c->id);
  cai_free_mem(NULL, c->session);
  cai_free_mem(NULL, c->command);
  cai_free_mem(NULL, c->line);
  cai_free_mem(NULL, c->first);
  cai_free_mem(NULL, c->last);
  cai_free_mem(NULL, c);
}
static int display_same(const char *a, const char *b) {
  return strcmp(a != NULL ? a : "", b != NULL ? b : "") == 0;
}
static cai_display_call *display_find(cai_tool_display *d,
                                      const cai_agent_runtime_event *e) {
  cai_display_call *c;
  for (c = d->calls; c != NULL; c = c->next)
    if (display_same(c->id, e->tool_call_id) &&
        display_same(c->session, e->runtime_session_id))
      return c;
  return NULL;
}
static int display_call_new(cai_tool_display *d,
                            const cai_agent_runtime_event *e,
                            cai_display_call **out, cai_error *error) {
  cai_display_call *c = (cai_display_call *)cai_alloc(NULL, sizeof(*c));
  if (c == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "cannot allocate tool display");
  memset(c, 0, sizeof(*c));
  c->id = cai_strdup(NULL, e->tool_call_id != NULL ? e->tool_call_id : "");
  c->session = cai_strdup(
      NULL, e->runtime_session_id != NULL ? e->runtime_session_id : "");
  if (c->id == NULL || c->session == NULL) {
    display_call_free(c);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "cannot copy tool display identity");
  }
  c->mode = d->config.mode;
  c->next = d->calls;
  d->calls = c;
  *out = c;
  return CAI_OK;
}
static void display_remove(cai_tool_display *d, cai_display_call *c) {
  cai_display_call **link;
  for (link = &d->calls; *link != NULL; link = &(*link)->next)
    if (*link == c) {
      *link = c->next;
      display_call_free(c);
      return;
    }
}
static int patch_piece(cai_tool_display *d, int kind, const char *s, size_t n,
                       int newline, cai_error *error) {
  int rc, colored;
  colored = d->config.color && (kind == '+' || kind == '-');
  rc = colored ? display_text(d, kind == '+' ? "\033[42m" : "\033[41m", error)
               : CAI_OK;
  if (rc == CAI_OK)
    rc = display_safe(d, s, n, error);
  if (rc == CAI_OK && colored)
    rc = display_text(d, "\033[0m", error);
  if (rc == CAI_OK && newline)
    rc = display_text(d, "\n", error);
  return rc;
}
static int patch_row(cai_tool_display *d, const char *s, size_t n, int newline,
                     cai_error *error) {
  return patch_piece(d, n != 0U ? s[0] : 0, s, n, newline, error);
}
static int patch_context(cai_tool_display *d, cai_display_call *c, int tail,
                         cai_error *error) {
  int rc = CAI_OK;
  if (c->context_lines != 0U) {
    if (!c->changed && !tail)
      rc = patch_row(d, c->last != NULL ? c->last : c->first,
                     c->last != NULL ? c->last_length : c->first_length, 1,
                     error);
    else {
      rc = patch_row(d, c->first, c->first_length, 1, error);
      if (rc == CAI_OK && !tail && c->context_lines > 2U)
        rc = display_text(d, " ⋮\n", error);
      if (rc == CAI_OK && !tail && c->context_lines > 1U)
        rc = patch_row(d, c->last, c->last_length, 1, error);
    }
  }
  cai_free_mem(NULL, c->first);
  cai_free_mem(NULL, c->last);
  c->first = NULL;
  c->last = NULL;
  c->context_lines = 0U;
  return rc;
}
static int patch_line(cai_tool_display *d, cai_display_call *c, const char *s,
                      size_t n, cai_error *error) {
  const char *verb = NULL, *path = NULL;
  int rc;
  if (n >= 14U && strncmp(s, "*** Add File: ", 14U) == 0) {
    verb = "Added";
    path = s + 14;
  } else if (n >= 17U && strncmp(s, "*** Update File: ", 17U) == 0) {
    verb = "Edited";
    path = s + 17;
  } else if (n >= 17U && strncmp(s, "*** Delete File: ", 17U) == 0) {
    verb = "Deleted";
    path = s + 17;
  } else if (n >= 13U && strncmp(s, "*** Move to: ", 13U) == 0) {
    verb = "Moved to";
    path = s + 13;
  }
  if (verb != NULL || (n >= 2U && s[0] == '@' && s[1] == '@') ||
      (n >= 3U && strncmp(s, "***", 3U) == 0)) {
    rc = patch_context(d, c, 1, error);
    c->changed = 0;
    if (rc != CAI_OK)
      return rc;
    if (verb != NULL)
      return display_marker(d, 1, verb, path, n - (size_t)(path - s), error);
    if (n >= 3U && strncmp(s, "***", 3U) == 0)
      return CAI_OK;
    return patch_row(d, s, n, 1, error);
  }
  if (c->mode == CAI_TOOL_OUTPUT_FULL)
    return patch_row(d, s, n, 1, error);
  if (n != 0U && (s[0] == '+' || s[0] == '-')) {
    rc = patch_context(d, c, 0, error);
    c->changed = 1;
    return rc == CAI_OK ? patch_row(d, s, n, 1, error) : rc;
  }
  if (n != 0U && s[0] == ' ') {
    char *copy = cai_strndup(NULL, s, n);
    if (copy == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "cannot retain patch context line");
    c->context_lines++;
    if (c->context_lines == 1U) {
      c->first = copy;
      c->first_length = n;
    } else {
      cai_free_mem(NULL, c->last);
      c->last = copy;
      c->last_length = n;
    }
    return CAI_OK;
  }
  rc = patch_context(d, c, 1, error);
  return rc == CAI_OK ? patch_row(d, s, n, 1, error) : rc;
}
static int patch_feed(cai_tool_display *d, cai_display_call *c,
                      const char *data, size_t n, int finish,
                      cai_error *error) {
  size_t i, flush_length;
  int rc;
  if (c->line == NULL) {
    c->line = (char *)cai_alloc(NULL, CAI_DISPLAY_LINE_MAX + 64U);
    if (c->line == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "cannot allocate patch line buffer");
  }
  for (i = 0U; i < n; i++) {
    if (data[i] == '\n') {
      if (c->long_line)
        rc = patch_piece(d, c->long_line, c->line, c->length, 1, error);
      else {
        if (c->clipped) {
          memcpy(c->line + c->length, " … [line clipped]",
                 sizeof(" … [line clipped]") - 1U);
          c->length += sizeof(" … [line clipped]") - 1U;
        }
        c->line[c->length] = '\0';
        rc = patch_line(d, c, c->line, c->length, error);
      }
      c->length = 0U;
      c->long_line = 0;
      c->clipped = 0;
      if (rc != CAI_OK)
        return rc;
    } else if (!c->clipped) {
      c->line[c->length++] = data[i];
      if (c->length == CAI_DISPLAY_LINE_MAX) {
        if (c->long_line || c->mode == CAI_TOOL_OUTPUT_FULL ||
            c->line[0] == '+' || c->line[0] == '-') {
          if (!c->long_line) {
            rc = patch_context(d, c, 0, error);
            c->changed = 1;
            if (rc != CAI_OK)
              return rc;
          }
          if (!c->long_line)
            c->long_line =
                c->line[0] == '+' || c->line[0] == '-' ? c->line[0] : ' ';
          flush_length = patch_flush_length(c->line, c->length);
          rc = patch_piece(d, c->long_line, c->line, flush_length, 0, error);
          if (rc != CAI_OK)
            return rc;
          memmove(c->line, c->line + flush_length, c->length - flush_length);
          c->length -= flush_length;
        } else
          c->clipped = 1;
      }
    }
  }
  if (finish) {
    if (c->length != 0U || c->long_line) {
      if (c->clipped) {
        memcpy(c->line + c->length, " … [line clipped]",
               sizeof(" … [line clipped]") - 1U);
        c->length += sizeof(" … [line clipped]") - 1U;
      }
      c->line[c->length] = '\0';
      rc = c->long_line
               ? patch_piece(d, c->long_line, c->line, c->length, 1, error)
               : patch_line(d, c, c->line, c->length, error);
      if (rc != CAI_OK)
        return rc;
    }
    return patch_context(d, c, 1, error);
  }
  return CAI_OK;
}
static int command_output(cai_tool_display *d, cai_display_call *c,
                          const char *s, size_t n, cai_error *error) {
  size_t i, start;
  int rc;
  start = 0U;
  for (i = 0U; i < n; i++) {
    if (c->mode == CAI_TOOL_OUTPUT_COMPACT &&
        c->lines >= d->config.preview_lines) {
      if (i > start) {
        rc = display_safe(d, s + start, i - start, error);
        if (rc != CAI_OK)
          return rc;
      }
      if (!c->omitted) {
        rc = display_text(
            d, " ⋮ (output omitted; /output full for future calls)\n", error);
        if (rc != CAI_OK)
          return rc;
        c->omitted = 1;
      }
      return CAI_OK;
    }
    c->partial_line = s[i] != '\n';
    if (s[i] == '\n')
      c->lines++;
  }
  return display_safe(d, s, n, error);
}
static int command_feed(cai_tool_display *d, cai_display_call *c, const char *s,
                        size_t n, cai_error *error) {
  size_t i, start;
  int rc;
  if (n == 0U)
    return CAI_OK;
  if (c->pending_cr) {
    c->pending_cr = 0;
    if (s[0] != '\n') {
      rc = command_output(d, c, "\r", 1U, error);
      if (rc != CAI_OK)
        return rc;
    }
  }
  start = 0U;
  for (i = 0U; i < n; i++) {
    if (s[i] != '\r')
      continue;
    rc = command_output(d, c, s + start, i - start, error);
    if (rc != CAI_OK)
      return rc;
    if (i + 1U == n)
      c->pending_cr = 1;
    else if (s[i + 1U] != '\n') {
      rc = command_output(d, c, "\r", 1U, error);
      if (rc != CAI_OK)
        return rc;
    }
    start = i + 1U;
  }
  return command_output(d, c, s + start, n - start, error);
}

int cai_tool_display_new(const cai_tool_display_config *config,
                         cai_tool_display **out, cai_error *error) {
  cai_tool_display *d;
  if (out != NULL)
    *out = NULL;
  if (config == NULL || config->sink == NULL || out == NULL ||
      (config->mode != CAI_TOOL_OUTPUT_COMPACT &&
       config->mode != CAI_TOOL_OUTPUT_FULL))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool display requires a sink and compact/full mode");
  d = (cai_tool_display *)cai_alloc(NULL, sizeof(*d));
  if (d == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "cannot allocate tool renderer");
  memset(d, 0, sizeof(*d));
  d->config = *config;
  if (d->config.preview_lines == 0U)
    d->config.preview_lines = 5U;
  *out = d;
  return CAI_OK;
}
int cai_tool_display_set_mode(cai_tool_display *d, int mode, cai_error *error) {
  if (d == NULL ||
      (mode != CAI_TOOL_OUTPUT_COMPACT && mode != CAI_TOOL_OUTPUT_FULL))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool output mode must be compact or full");
  d->config.mode = mode;
  return CAI_OK;
}
int cai_tool_display_event(cai_tool_display *d,
                           const cai_agent_runtime_event *e, cai_error *error) {
  cai_display_call *c;
  const char *text, *verb;
  size_t n;
  int rc, ok;
  if (d == NULL || e == NULL || (e->data == NULL && e->data_length != 0U))
    return cai_set_error(error, CAI_ERR_INVALID, "invalid tool display event");
  c = display_find(d, e);
  if (e->type == CAI_AGENT_EVENT_RUN_FAILED ||
      e->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
    cai_display_call *next;
    for (c = d->calls; c != NULL; c = next) {
      next = c->next;
      if (display_same(c->session, e->runtime_session_id))
        display_remove(d, c);
    }
    return CAI_OK;
  }
  if (e->type == CAI_AGENT_EVENT_TOOL_CALL_STARTED) {
    if (c != NULL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "tool display call already active");
    rc = display_call_new(d, e, &c, error);
    if (rc != CAI_OK)
      return rc;
    return CAI_OK;
  }
  if (e->type == CAI_AGENT_EVENT_TOOL_PATCH) {
    if (c == NULL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "patch display has no originating call");
    rc = patch_feed(d, c, e->data, e->data_length, e->data_length == 0U, error);
    if (e->data_length == 0U)
      display_remove(d, c);
    return rc;
  }
  if (e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED) {
    if (c == NULL) {
      rc = display_call_new(d, e, &c, error);
      if (rc != CAI_OK)
        return rc;
    }
    if (c->command_active)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "terminal command already started");
    c->command =
        cai_strndup(NULL, e->data != NULL ? e->data : "", e->data_length);
    if (c->command == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "cannot retain command display label");
    c->command_active = 1;
    return display_marker(d, -1, "Running", c->command, strlen(c->command),
                          error);
  }
  if (e->type == CAI_AGENT_EVENT_TERMINAL_OUTPUT)
    return c != NULL ? command_feed(d, c, e->data, e->data_length, error)
                     : CAI_OK;
  if (e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED ||
      e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED) {
    if (c == NULL)
      return CAI_OK;
    if (!c->command_active)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "terminal completion has no started command");
    rc = command_feed(d, c, e->data, e->data_length, error);
    if (rc == CAI_OK && c->pending_cr)
      rc = command_output(d, c, "\r", 1U, error);
    if (rc == CAI_OK && c->partial_line)
      rc = display_text(d, "\n", error);
    if (rc == CAI_OK && e->terminal_output_truncated)
      rc = display_text(
          d, "  [command output truncated by the tool's output limit]\n",
          error);
    ok = e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED &&
         e->terminal_has_exit_code && e->terminal_exit_code == 0 &&
         (!e->terminal_has_signal || e->terminal_signal == 0);
    if (rc == CAI_OK)
      rc = display_marker(d, ok,
                          e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED
                              ? "Cancelled"
                              : "Ran",
                          c->command, strlen(c->command), error);
    if (rc == CAI_OK && !ok) {
      char status[96];
      snprintf(status, sizeof(status), "  exit=%lld signal=%lld\n",
               e->terminal_has_exit_code ? e->terminal_exit_code : -1LL,
               e->terminal_has_signal ? e->terminal_signal : 0LL);
      rc = display_text(d, status, error);
    }
    display_remove(d, c);
    return rc;
  }
  if (e->type != CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
      e->type != CAI_AGENT_EVENT_TOOL_CALL_FAILED)
    return CAI_OK;
  ok = e->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
  if (ok && e->tool_action == CAI_AGENT_TOOL_ACTION_MCP &&
      display_mcp_failed(e))
    ok = 0;
  if (ok && e->tool_action == CAI_AGENT_TOOL_ACTION_PATCH)
    return CAI_OK;
  if (c != NULL && c->command_active)
    return CAI_OK;
  if (ok && (e->tool_action == CAI_AGENT_TOOL_ACTION_EXECUTE ||
             e->tool_action == CAI_AGENT_TOOL_ACTION_WRITE_STDIN)) {
    if (c != NULL)
      display_remove(d, c);
    return CAI_OK;
  }
  verb = !ok ? (e->tool_action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP failed"
                                                            : "Failed")
         : e->tool_action == CAI_AGENT_TOOL_ACTION_READ   ? "Read"
         : e->tool_action == CAI_AGENT_TOOL_ACTION_LIST   ? "Listed"
         : e->tool_action == CAI_AGENT_TOOL_ACTION_VIEW   ? "Viewed"
         : e->tool_action == CAI_AGENT_TOOL_ACTION_CHROME ? "Used Chrome"
         : e->tool_action == CAI_AGENT_TOOL_ACTION_MCP    ? "MCP"
                                                          : "Completed";
  text = e->tool_path != NULL   ? e->tool_path
         : e->tool_name != NULL ? e->tool_name
                                : "tool";
  rc = display_marker(d, ok, verb, text, strlen(text), error);
  if (rc == CAI_OK &&
      (!ok || e->tool_action == CAI_AGENT_TOOL_ACTION_MCP ||
       e->tool_action == CAI_AGENT_TOOL_ACTION_EXTERNAL) &&
      e->data_length != 0U) {
    if (c != NULL)
      rc = command_output(d, c, e->data, e->data_length, error);
    else
      rc = display_safe(d, e->data, e->data_length, error);
    n = e->data_length;
    if (rc == CAI_OK && e->data[n - 1U] != '\n')
      rc = display_text(d, "\n", error);
  }
  if (c != NULL)
    display_remove(d, c);
  return rc;
}
void cai_tool_display_close(cai_tool_display *d) {
  cai_display_call *c;
  if (d == NULL)
    return;
  while ((c = d->calls) != NULL) {
    d->calls = c->next;
    display_call_free(c);
  }
  cai_free_mem(NULL, d);
}
int cai_patch_render(cai_source *patch, const cai_tool_display_config *config,
                     cai_error *error) {
  cai_tool_display *d;
  cai_display_call c;
  cai_error local_error;
  char bytes[4096];
  size_t n;
  int rc;
  if (patch == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "patch source is required");
  rc = cai_tool_display_new(config, &d, error);
  if (rc != CAI_OK)
    return rc;
  cai_error_init(&local_error);
  if (error == NULL)
    error = &local_error;
  memset(&c, 0, sizeof(c));
  c.mode = config->mode;
  do {
    n = cai_source_read(patch, bytes, sizeof(bytes), error);
    if (n > sizeof(bytes)) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "patch source exceeded read capacity");
      break;
    }
    if (error != NULL && error->code != CAI_OK) {
      rc = error->code;
      break;
    }
    rc = patch_feed(d, &c, bytes, n, n == 0U, error);
  } while (rc == CAI_OK && n != 0U);
  cai_free_mem(NULL, c.line);
  cai_free_mem(NULL, c.first);
  cai_free_mem(NULL, c.last);
  cai_tool_display_close(d);
  cai_error_cleanup(&local_error);
  return rc;
}
