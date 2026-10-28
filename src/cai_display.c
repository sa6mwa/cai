#define _XOPEN_SOURCE 700
#include "cai_internal.h"
#include <cai/display.h>
#include <wchar.h>

#define CAI_DISPLAY_LINE_MAX 65536U
#define COMMAND_SGR_SLOTS 16U
#define COMMAND_SGR_SIZE 40U
enum {
  COMMAND_TEXT,
  COMMAND_ESCAPE,
  COMMAND_INTERMEDIATE,
  COMMAND_CSI,
  COMMAND_STRING,
  COMMAND_STRING_ESCAPE
};

typedef struct patch_location {
  size_t old_line, new_line;
  struct patch_location *next;
} patch_location;
typedef struct cai_display_call {
  char *id;
  char *session;
  char *command;
  int mode;
  int command_active;
  int replay;
  size_t lines;
  int partial_line;
  int pending_cr;
  int omitted;
  /* Command controls and styles belong to the originating call, including
   * across wait/write_stdin events. No command output is retained. */
  unsigned int control_state;
  int osc, control_invalid, style_open, style_dirty;
  char control[128], style[COMMAND_SGR_SLOTS][COMMAND_SGR_SIZE];
  size_t control_length;
  unsigned char command_utf8[4];
  size_t command_utf8_length, command_utf8_need;
  char *line;
  size_t length;
  char *first;
  char *last;
  size_t first_length;
  size_t last_length;
  size_t context_lines;
  int had_hunk;
  int changed;
  int long_line;
  int clipped;
  patch_location *locations, *locations_tail;
  char location_row[64];
  size_t location_length;
  int locations_done, patch_started;
  size_t old_line, new_line, first_number, last_number;
  size_t row_width, row_column, row_prefix;
  int row_active, row_kind;
  char row_bytes[512];
  size_t row_length;
  struct cai_display_call *next;
} cai_display_call;

struct cai_tool_display {
  cai_tool_display_config config;
  cai_display_call *calls;
  size_t width;
  unsigned char utf8[4];
  size_t utf8_length;
  size_t utf8_expected;
  unsigned int breaks;
  int ansi;
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
  size_t i;
  int rc;
  if (n == 0U)
    return CAI_OK;
  rc = cai_sink_write(d->config.sink, s, n, error);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (d->ansi == 1)
      d->ansi = ch == '[' ? 2 : 0;
    else if (d->ansi == 2) {
      if (ch >= 0x40U && ch <= 0x7eU)
        d->ansi = 0;
    } else if (ch == 27U)
      d->ansi = 1;
    else if (ch == '\n') {
      if (d->breaks < 2U)
        d->breaks++;
    } else if (ch != '\r')
      d->breaks = 0U;
  }
  return CAI_OK;
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
static void display_command_verb(char out[80], const char *verb,
                                 unsigned long long duration_ms) {
  unsigned long long seconds = duration_ms / 1000U;
  const char *elapsed = strcmp(verb, "Cancelled") == 0 ? "after" : "for";
  if (seconds >= 3600U)
    snprintf(out, 80U, "%s %s %lluh%llum%llus", verb, elapsed, seconds / 3600U,
             (seconds / 60U) % 60U, seconds % 60U);
  else if (seconds >= 60U)
    snprintf(out, 80U, "%s %s %llum%llus", verb, elapsed, seconds / 60U,
             seconds % 60U);
  else
    snprintf(out, 80U, "%s %s %llus", verb, elapsed, seconds);
}
static int display_marker(cai_tool_display *d, int result, const char *verb,
                          const char *text, size_t n, cai_error *error) {
  int rc;
  const char *star = !d->config.color || result < 0 ? "* "
                     : result                       ? "\033[92m*\033[0m "
                                                    : "\033[91m*\033[0m ";
  rc = display_flush_utf8(d, error);
  if (rc == CAI_OK && d->config.begin_block != NULL)
    rc = d->config.begin_block(d->config.begin_block_context, error);
  else if (rc == CAI_OK) {
    while (rc == CAI_OK && d->breaks < 2U)
      rc = display_text(d, "\n", error);
  }
  if (rc == CAI_OK)
    rc = display_text(d, star, error);
  if (rc == CAI_OK && d->config.color)
    rc = display_text(d, "\033[0;36m", error);
  if (rc == CAI_OK)
    rc = display_safe(d, verb, strlen(verb), error);
  if (rc == CAI_OK && n != 0U)
    rc = display_text(d, " ", error);
  if (rc == CAI_OK && n != 0U && d->config.color)
    rc = display_text(d, "\033[1;96m", error);
  if (rc == CAI_OK)
    rc = display_safe(d, text, n, error);
  if (rc == CAI_OK)
    rc = display_flush_utf8(d, error);
  if (rc == CAI_OK && d->config.color)
    rc = display_text(d, "\033[0m", error);
  if (rc == CAI_OK)
    rc = display_text(d, "\n", error);
  return rc;
}
static void display_call_free(cai_display_call *c) {
  while (c->locations != NULL) {
    patch_location *next = c->locations->next;
    cai_free_mem(NULL, c->locations);
    c->locations = next;
  }
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
/* Width-aware output retains at most one small byte chunk. In particular a
 * long logical line never becomes a retained, reflowable whole value. */
static int patch_output(cai_tool_display *d, cai_display_call *c, const char *s,
                        size_t n, cai_error *error) {
  int rc;
  if (n > sizeof(c->row_bytes) - c->row_length) {
    rc = display_write(d, c->row_bytes, c->row_length, error);
    c->row_length = 0U;
    if (rc != CAI_OK)
      return rc;
  }
  memcpy(c->row_bytes + c->row_length, s, n);
  c->row_length += n;
  return CAI_OK;
}
static int patch_row_end(cai_tool_display *d, cai_display_call *c,
                         cai_error *error) {
  int rc = CAI_OK;
  int colored = d->config.color && (c->row_kind == '+' || c->row_kind == '-');
  while (colored && rc == CAI_OK && c->row_column < c->row_width) {
    rc = patch_output(d, c, " ", 1U, error);
    c->row_column++;
  }
  if (rc == CAI_OK && colored)
    rc = patch_output(d, c, "\033[0m", 4U, error);
  if (rc == CAI_OK)
    rc = patch_output(d, c, "\n", 1U, error);
  if (rc == CAI_OK)
    rc = display_write(d, c->row_bytes, c->row_length, error);
  c->row_length = 0U;
  return rc;
}
static int patch_row_start(cai_tool_display *d, cai_display_call *c,
                           size_t number, int continuation, cai_error *error) {
  char prefix[64];
  size_t n;
  int rc = CAI_OK;
  int colored = d->config.color && (c->row_kind == '+' || c->row_kind == '-');
  if (colored) {
    const char *style = c->row_kind == '+' ? "\033[48;2;33;58;43m\033[97m"
                                           : "\033[48;2;74;34;29m\033[97m";
    rc = patch_output(d, c, style, strlen(style), error);
  }
  if (continuation) {
    memset(prefix, ' ', c->row_prefix);
    n = c->row_prefix;
  } else {
    if (number != 0U)
      snprintf(prefix, sizeof(prefix), "%4lu %c", (unsigned long)number,
               c->row_kind);
    else
      snprintf(prefix, sizeof(prefix), "     %c", c->row_kind);
    n = strlen(prefix);
    c->row_prefix = n;
  }
  c->row_column = n;
  if (rc == CAI_OK && colored && !continuation) {
    rc = patch_output(d, c, prefix, n - 1U, error);
    if (rc == CAI_OK)
      rc = patch_output(d, c, c->row_kind == '+' ? "\033[92m" : "\033[91m", 5U,
                        error);
    if (rc == CAI_OK)
      rc = patch_output(d, c, prefix + n - 1U, 1U, error);
    if (rc == CAI_OK)
      rc = patch_output(d, c, "\033[97m", 5U, error);
  } else if (rc == CAI_OK)
    rc = patch_output(d, c, prefix, n, error);
  return rc;
}
static int patch_glyph(cai_tool_display *d, cai_display_call *c, const char *s,
                       size_t n, size_t cells, cai_error *error) {
  int rc = CAI_OK;
  if (cells != 0U && c->row_column + cells > c->row_width) {
    rc = patch_row_end(d, c, error);
    if (rc == CAI_OK)
      rc = patch_row_start(d, c, 0U, 1, error);
  }
  if (rc == CAI_OK)
    rc = patch_output(d, c, s, n, error);
  c->row_column += cells;
  return rc;
}
static int patch_piece(cai_tool_display *d, cai_display_call *c, int kind,
                       size_t number, const char *s, size_t n, int newline,
                       cai_error *error) {
  int rc = CAI_OK, colored;
  size_t i = 0U;
  if ((c->row_active ? c->row_width : d->width) == 0U) {
    int first = !c->row_active;
    c->row_width = 0U;
    c->row_active = !newline;
    colored = d->config.color && (kind == '+' || kind == '-');
    rc = colored ? display_text(d,
                                kind == '+' ? "\033[48;2;33;58;43m\033[97m"
                                            : "\033[48;2;74;34;29m\033[97m",
                                error)
                 : CAI_OK;
    if (rc == CAI_OK && colored && first && n != 0U) {
      rc = display_text(d, kind == '+' ? "\033[92m" : "\033[91m", error);
      if (rc == CAI_OK)
        rc = display_safe(d, s, 1U, error);
      if (rc == CAI_OK)
        rc = display_text(d, "\033[97m", error);
      s++;
      n--;
    }
    if (rc == CAI_OK)
      rc = display_safe(d, s, n, error);
    if (rc == CAI_OK && colored)
      rc = display_text(d, "\033[0m", error);
    if (rc == CAI_OK && newline)
      rc = display_text(d, "\n", error);
    return rc;
  }
  if (!c->row_active) {
    c->row_active = 1;
    c->row_kind = kind;
    c->row_width = d->width;
    rc = patch_row_start(d, c, number, 0, error);
    if (n != 0U)
      i = 1U; /* marker already present in the gutter */
  }
  while (rc == CAI_OK && i < n) {
    unsigned char lead = (unsigned char)s[i];
    size_t need = lead < 128U                      ? 1U
                  : lead >= 0xc2U && lead < 0xe0U  ? 2U
                  : lead >= 0xe0U && lead < 0xf0U  ? 3U
                  : lead >= 0xf0U && lead <= 0xf4U ? 4U
                                                   : 0U;
    size_t j;
    unsigned long code = need == 1U ? lead : lead & (0x7fU >> need);
    int valid = need != 0U && need <= n - i, cells;
    for (j = 1U; valid && j < need; j++) {
      unsigned char ch = (unsigned char)s[i + j];
      if ((ch & 0xc0U) != 0x80U)
        valid = 0;
      code = (code << 6) | (ch & 0x3fU);
    }
    valid = valid && !(need == 2U && code < 128U) &&
            !(need == 3U && code < 2048U) && !(need == 4U && code < 65536U) &&
            !(code >= 0xd800U && code <= 0xdfffU) && code <= 0x10ffffU;
    if (lead == '\t') {
      size_t spaces = 8U - (c->row_column - c->row_prefix) % 8U;
      for (j = 0U; rc == CAI_OK && j < spaces; j++)
        rc = patch_glyph(d, c, " ", 1U, 1U, error);
      i++;
    } else if (!valid || code < 32U || (code >= 127U && code < 160U)) {
      char escaped[5];
      snprintf(escaped, sizeof(escaped), "\\x%02x", (unsigned)lead);
      for (j = 0U; rc == CAI_OK && j < 4U; j++)
        rc = patch_glyph(d, c, escaped + j, 1U, 1U, error);
      i++;
    } else {
      cells = wcwidth((wchar_t)code);
      if (cells < 0)
        cells = 1;
      rc = patch_glyph(d, c, s + i, need, (size_t)cells, error);
      i += need;
    }
  }
  if (rc == CAI_OK && newline) {
    rc = patch_row_end(d, c, error);
    c->row_active = 0;
  } else if (rc == CAI_OK) {
    rc = display_write(d, c->row_bytes, c->row_length, error);
    c->row_length = 0U;
  }
  return rc;
}
static size_t patch_number(cai_display_call *c, int kind) {
  size_t number = kind == '-' ? c->old_line : c->new_line;
  if (kind != '+' && c->old_line != 0U && c->old_line < (size_t)-1)
    c->old_line++;
  if (kind != '-' && c->new_line != 0U && c->new_line < (size_t)-1)
    c->new_line++;
  return number;
}
static int patch_row(cai_tool_display *d, cai_display_call *c, const char *s,
                     size_t n, size_t number, cai_error *error) {
  return patch_piece(d, c, n != 0U ? s[0] : ' ', number, s, n, 1, error);
}
static int patch_context(cai_tool_display *d, cai_display_call *c, int tail,
                         cai_error *error) {
  int rc = CAI_OK;
  if (c->context_lines != 0U) {
    if (!c->changed && !tail)
      rc = patch_row(d, c, c->last != NULL ? c->last : c->first,
                     c->last != NULL ? c->last_length : c->first_length,
                     c->last != NULL ? c->last_number : c->first_number, error);
    else {
      rc = patch_row(d, c, c->first, c->first_length, c->first_number, error);
      if (rc == CAI_OK && !tail && c->context_lines > 2U)
        rc = display_text(d, d->width != 0U ? "      ⋮\n" : " ⋮\n", error);
      if (rc == CAI_OK && !tail && c->context_lines > 1U)
        rc = patch_row(d, c, c->last, c->last_length, c->last_number, error);
    }
  }
  cai_free_mem(NULL, c->first);
  cai_free_mem(NULL, c->last);
  c->first = NULL;
  c->last = NULL;
  c->context_lines = 0U;
  return rc;
}
static void patch_header_location(cai_display_call *c, const char *s,
                                  size_t n) {
  size_t pos = 4U, field, values[2] = {0U, 0U};
  c->old_line = c->new_line = 0U;
  if (n < 4U || memcmp(s, "@@ -", 4U) != 0)
    return;
  for (field = 0U; field < 2U; field++) {
    size_t start = pos;
    while (pos < n && s[pos] >= '0' && s[pos] <= '9') {
      size_t digit = (size_t)(s[pos++] - '0');
      if (values[field] > ((size_t)-1 - digit) / 10U)
        return;
      values[field] = values[field] * 10U + digit;
    }
    if (start == pos || values[field] == 0U)
      return;
    if (field == 0U) {
      if (n - pos < 2U || memcmp(s + pos, " +", 2U) != 0)
        return;
      pos += 2U;
    }
  }
  if (n - pos != 3U || memcmp(s + pos, " @@", 3U) != 0)
    return;
  c->old_line = values[0];
  c->new_line = values[1];
}
static int patch_line(cai_tool_display *d, cai_display_call *c, const char *s,
                      size_t n, cai_error *error) {
  const char *verb = NULL, *path = NULL;
  size_t number;
  size_t previous_line;
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
    if (verb != NULL) {
      c->had_hunk = 0;
      if (strcmp(verb, "Added") == 0) {
        c->old_line = 0U;
        c->new_line = 1U;
      } else if (strcmp(verb, "Moved to") != 0)
        c->old_line = c->new_line = 0U;
      return display_marker(d, 1, verb, path, n - (size_t)(path - s), error);
    }
    if (n >= 3U && strncmp(s, "***", 3U) == 0)
      return CAI_OK;
    previous_line = c->new_line;
    if (c->locations != NULL) {
      patch_location *location = c->locations;
      c->old_line = location->old_line;
      c->new_line = location->new_line;
      c->locations = location->next;
      if (c->locations == NULL)
        c->locations_tail = NULL;
      cai_free_mem(NULL, location);
    } else
      patch_header_location(c, s, n);
    if (c->mode == CAI_TOOL_OUTPUT_COMPACT && c->had_hunk &&
        previous_line != 0U && c->new_line > previous_line) {
      rc = display_text(d, d->width != 0U ? "      ⋮\n" : " ⋮\n", error);
      if (rc != CAI_OK)
        return rc;
    }
    c->had_hunk = 1;
    return d->width != 0U ? CAI_OK : patch_piece(d, c, 0, 0U, s, n, 1, error);
  }
  number = patch_number(c, n != 0U ? s[0] : ' ');
  if (c->mode == CAI_TOOL_OUTPUT_FULL)
    return patch_row(d, c, s, n, number, error);
  if (n != 0U && (s[0] == '+' || s[0] == '-')) {
    rc = patch_context(d, c, 0, error);
    c->changed = 1;
    return rc == CAI_OK ? patch_row(d, c, s, n, number, error) : rc;
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
      c->first_number = number;
    } else {
      cai_free_mem(NULL, c->last);
      c->last = copy;
      c->last_length = n;
      c->last_number = number;
    }
    return CAI_OK;
  }
  rc = patch_context(d, c, 1, error);
  return rc == CAI_OK ? patch_row(d, c, s, n, number, error) : rc;
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
        rc = patch_piece(d, c, c->long_line, 0U, c->line, c->length, 1, error);
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
          if (!c->long_line) {
            c->long_line =
                c->line[0] == '+' || c->line[0] == '-' ? c->line[0] : ' ';
            c->first_number = patch_number(c, c->long_line);
          }
          flush_length = patch_flush_length(c->line, c->length);
          rc = patch_piece(d, c, c->long_line, c->first_number, c->line,
                           flush_length, 0, error);
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
      rc = c->long_line ? patch_piece(d, c, c->long_line, 0U, c->line,
                                      c->length, 1, error)
                        : patch_line(d, c, c->line, c->length, error);
      if (rc != CAI_OK)
        return rc;
    }
    return patch_context(d, c, 1, error);
  }
  return CAI_OK;
}
/* Restore only this command's SGR attributes; always reset them before
 * returning to the host, so assistant text and interleaved calls cannot inherit
 * them. */
static int command_styled(cai_tool_display *d, cai_display_call *c,
                          const char *s, size_t n, cai_error *error) {
  size_t i;
  int rc = CAI_OK;
  if (n == 0U)
    return CAI_OK;
  if (!c->command_active)
    return display_safe(d, s, n, error);
  if (d->config.color && (!c->style_open || c->style_dirty)) {
    if (c->style_open)
      rc = display_text(d, "\033[0m", error);
    c->style_open = 0;
    for (i = 0U; rc == CAI_OK && i < COMMAND_SGR_SLOTS; i++) {
      if (c->style[i][0] == '\0')
        continue;
      rc = display_text(d, "\033[", error);
      if (rc == CAI_OK)
        rc = display_text(d, c->style[i], error);
      if (rc == CAI_OK)
        rc = display_text(d, "m", error);
      c->style_open = 1;
    }
    c->style_dirty = 0;
  }
  return rc == CAI_OK ? display_safe(d, s, n, error) : rc;
}
static int command_style_end(cai_tool_display *d, cai_display_call *c,
                             cai_error *error) {
  if (!c->style_open)
    return CAI_OK;
  c->style_open = 0;
  return display_text(d, "\033[0m", error);
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
        rc = command_styled(d, c, s + start, i - start, error);
        if (rc != CAI_OK)
          return rc;
      }
      if (!c->omitted) {
        rc = command_style_end(d, c, error);
        if (rc != CAI_OK)
          return rc;
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
  return command_styled(d, c, s, n, error);
}

/* Canonical attribute slots bound style state even for an endless stream of
 * SGR changes without resets. Unknown SGR attributes are ignored. */
static void command_sgr(cai_display_call *c) {
  size_t starts[32], ends[32], count = 0U, i = 0U, j, end;
  unsigned int codes[32], code, mode;
  char next[COMMAND_SGR_SLOTS][COMMAND_SGR_SIZE];
  int slot;
  if (c->control_invalid)
    return;
  do {
    if (count == 32U)
      return;
    starts[count] = i;
    code = 0U;
    while (i < c->control_length && c->control[i] != ';') {
      if (c->control[i] == ':') {
        while (i < c->control_length && c->control[i] != ';')
          i++;
        break;
      }
      if (code > 1000U)
        return;
      code = code * 10U + (unsigned int)(c->control[i++] - '0');
    }
    codes[count] = code;
    ends[count++] = i;
  } while (i++ < c->control_length);
  memcpy(next, c->style, sizeof(next));
  for (i = 0U; i < count; i++) {
    code = codes[i];
    end = ends[i];
    slot = -1;
    if (code == 0U)
      memset(next, 0, sizeof(next));
    else if (code == 1U || code == 2U)
      slot = (int)code - 1;
    else if (code == 3U || code == 20U)
      slot = 2;
    else if (code == 4U || code == 21U)
      slot = 3;
    else if (code == 5U || code == 6U)
      slot = 4;
    else if (code >= 7U && code <= 9U)
      slot = (int)code - 2;
    else if (code >= 11U && code <= 19U)
      slot = 8;
    else if (code == 51U || code == 52U)
      slot = 9;
    else if (code == 53U)
      slot = 10;
    else if (code == 73U || code == 74U)
      slot = 11;
    else if ((code >= 30U && code <= 37U) || (code >= 90U && code <= 97U))
      slot = 12;
    else if ((code >= 40U && code <= 47U) || (code >= 100U && code <= 107U))
      slot = 13;
    else if (code == 26U)
      slot = 15;
    else if (code == 38U || code == 48U || code == 58U) {
      slot = code == 38U ? 12 : code == 48U ? 13 : 14;
      if (memchr(c->control + starts[i], ':', end - starts[i]) == NULL) {
        if (i + 1U == count)
          return;
        mode = codes[i + 1U];
        j = mode == 2U ? 4U : mode == 5U ? 2U : 0U;
        if (j == 0U || j >= count - i)
          return;
        end = ends[i + j];
        for (j = i + 2U; j < count && ends[j] <= end; j++)
          if (codes[j] > 255U)
            return;
      }
    } else {
      if (code == 22U)
        next[0][0] = next[1][0] = '\0';
      else if (code == 23U)
        next[2][0] = '\0';
      else if (code == 24U)
        next[3][0] = '\0';
      else if (code == 25U)
        next[4][0] = '\0';
      else if (code >= 27U && code <= 29U)
        next[code - 22U][0] = '\0';
      else if (code == 10U)
        next[8][0] = '\0';
      else if (code == 54U)
        next[9][0] = '\0';
      else if (code == 55U)
        next[10][0] = '\0';
      else if (code == 75U)
        next[11][0] = '\0';
      else if (code == 39U)
        next[12][0] = '\0';
      else if (code == 49U)
        next[13][0] = '\0';
      else if (code == 59U)
        next[14][0] = '\0';
      else if (code == 50U)
        next[15][0] = '\0';
    }
    if (slot >= 0) {
      if (end - starts[i] >= COMMAND_SGR_SIZE)
        return;
      memcpy(next[slot], c->control + starts[i], end - starts[i]);
      next[slot][end - starts[i]] = '\0';
      while (ends[i] < end)
        i++;
    }
  }
  memcpy(c->style, next, sizeof(next));
  c->style_dirty = 1;
}

typedef struct command_writer {
  cai_tool_display *display;
  cai_display_call *call;
  cai_error *error;
  char bytes[4096];
  size_t length;
} command_writer;
static int command_flush(command_writer *w) {
  int rc = command_output(w->display, w->call, w->bytes, w->length, w->error);
  w->length = 0U;
  return rc;
}
static int command_append(command_writer *w, const char *s, size_t n) {
  int rc;
  if (n > sizeof(w->bytes) - w->length) {
    rc = command_flush(w);
    if (rc != CAI_OK)
      return rc;
  }
  memcpy(w->bytes + w->length, s, n);
  w->length += n;
  return CAI_OK;
}
static int command_hex(command_writer *w, unsigned char ch) {
  char escaped[5];
  snprintf(escaped, sizeof(escaped), "\\x%02x", (unsigned)ch);
  return command_append(w, escaped, 4U);
}
static int command_control(command_writer *w, unsigned char ch) {
  cai_display_call *c = w->call;
  int rc;
  if (c->control_state == COMMAND_STRING ||
      c->control_state == COMMAND_STRING_ESCAPE) {
    if (ch == 156U || (c->osc && ch == 7U) || ch == 24U || ch == 26U ||
        (c->control_state == COMMAND_STRING_ESCAPE && ch == '\\'))
      c->control_state = COMMAND_TEXT;
    else
      c->control_state = ch == 27U ? COMMAND_STRING_ESCAPE : COMMAND_STRING;
    return CAI_OK;
  }
  if (ch == 27U) {
    rc = command_flush(w);
    c->control_state = COMMAND_ESCAPE;
    return rc;
  }
  if (ch == 155U || ch == 144U || ch == 152U || ch == 157U || ch == 158U ||
      ch == 159U) {
    rc = command_flush(w);
    c->control_state = ch == 155U ? COMMAND_CSI : COMMAND_STRING;
    c->osc = ch == 157U;
    c->control_length = 0U;
    c->control_invalid = 0;
    return rc;
  }
  if (c->control_state == COMMAND_ESCAPE) {
    if (ch == '[') {
      c->control_state = COMMAND_CSI;
      c->control_length = 0U;
      c->control_invalid = 0;
    } else if (ch == ']' || ch == 'P' || ch == 'X' || ch == '^' || ch == '_') {
      c->control_state = COMMAND_STRING;
      c->osc = ch == ']';
    } else
      c->control_state =
          ch >= 32U && ch <= 47U ? COMMAND_INTERMEDIATE : COMMAND_TEXT;
    return CAI_OK;
  }
  if (c->control_state == COMMAND_CSI ||
      c->control_state == COMMAND_INTERMEDIATE) {
    if (ch == 24U || ch == 26U)
      c->control_state = COMMAND_TEXT;
    else if (ch >= 64U && ch <= 126U) {
      if (c->control_state == COMMAND_CSI && ch == 'm')
        command_sgr(c);
      c->control_state = COMMAND_TEXT;
    } else if (c->control_state == COMMAND_INTERMEDIATE && ch >= 48U)
      c->control_state = COMMAND_TEXT;
    else if (c->control_state == COMMAND_CSI) {
      if ((ch >= '0' && ch <= '9') || ch == ';' || ch == ':') {
        if (c->control_length < sizeof(c->control))
          c->control[c->control_length++] = (char)ch;
        else
          c->control_invalid = 1;
      } else
        c->control_invalid = 1;
    }
    return CAI_OK;
  }
  if (ch == '\r' || ch == '\n') {
    rc = ch == '\n' && c->pending_cr ? CAI_OK : command_append(w, "\n", 1U);
    c->pending_cr = ch == '\r';
    return rc;
  }
  if (ch >= 32U && ch != 127U && (ch < 128U || ch >= 160U)) {
    c->pending_cr = 0;
    return ch < 128U ? command_append(w, (const char *)&ch, 1U)
                     : command_hex(w, ch);
  }
  if (ch == '\t') {
    c->pending_cr = 0;
    return command_append(w, "\t", 1U);
  }
  return CAI_OK;
}
static int command_invalid_utf8(command_writer *w) {
  cai_display_call *c = w->call;
  size_t i;
  int rc = CAI_OK;
  if (c->control_state == COMMAND_TEXT)
    for (i = 0U; rc == CAI_OK && i < c->command_utf8_length; i++)
      rc = command_hex(w, c->command_utf8[i]);
  c->command_utf8_length = 0U;
  return rc;
}
static int command_byte(command_writer *w, unsigned char ch) {
  cai_display_call *c = w->call;
  unsigned long scalar;
  size_t i;
  int rc;
  if (c->command_utf8_length != 0U) {
    if ((ch & 192U) != 128U) {
      rc = command_invalid_utf8(w);
      return rc == CAI_OK ? command_byte(w, ch) : rc;
    }
    c->command_utf8[c->command_utf8_length++] = ch;
    if (c->command_utf8_length != c->command_utf8_need)
      return CAI_OK;
    scalar = c->command_utf8[0] & ((1U << (7U - c->command_utf8_need)) - 1U);
    for (i = 1U; i < c->command_utf8_need; i++)
      scalar = (scalar << 6) | (c->command_utf8[i] & 63U);
    if ((c->command_utf8_need == 2U && scalar < 128UL) ||
        (c->command_utf8_need == 3U && scalar < 2048UL) ||
        (c->command_utf8_need == 4U && scalar < 65536UL) ||
        scalar > 1114111UL || (scalar >= 55296UL && scalar <= 57343UL))
      return command_invalid_utf8(w);
    c->command_utf8_length = 0U;
    if (scalar >= 128UL && scalar <= 159UL)
      return command_control(w, (unsigned char)scalar);
    if (c->control_state != COMMAND_TEXT) {
      if (c->control_state == COMMAND_CSI)
        c->control_invalid = 1;
      return CAI_OK;
    }
    c->pending_cr = 0;
    return command_append(w, (const char *)c->command_utf8,
                          c->command_utf8_need);
  }
  if (ch >= 194U && ch <= 244U) {
    c->command_utf8[0] = ch;
    c->command_utf8_length = 1U;
    c->command_utf8_need = ch < 224U ? 2U : ch < 240U ? 3U : 4U;
    if (c->control_state == COMMAND_STRING_ESCAPE)
      c->control_state = COMMAND_STRING;
    return CAI_OK;
  }
  return command_control(w, ch);
}
static int command_feed(cai_tool_display *d, cai_display_call *c, const char *s,
                        size_t n, int finish, cai_error *error) {
  command_writer w;
  size_t i;
  int rc = CAI_OK;
  if (c->omitted)
    return CAI_OK;
  w.display = d;
  w.call = c;
  w.error = error;
  w.length = 0U;
  for (i = 0U; rc == CAI_OK && i < n; i++)
    rc = command_byte(&w, (unsigned char)s[i]);
  if (rc == CAI_OK && finish)
    rc = command_invalid_utf8(&w);
  if (rc == CAI_OK)
    rc = command_flush(&w);
  if (rc == CAI_OK)
    rc = command_style_end(d, c, error);
  return rc;
}

static int patch_locations(cai_display_call *c, const char *data, size_t n,
                           cai_error *error) {
  size_t i;
  if (c->locations_done || c->patch_started)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "patch locations arrived after their boundary");
  for (i = 0U; i < n; i++) {
    if (data[i] == '\n') {
      patch_location *location;
      size_t values[2] = {0U, 0U}, field, pos = 0U;
      c->location_row[c->location_length] = '\0';
      for (field = 0U; field < 2U; field++) {
        size_t start = pos;
        while (pos < c->location_length && c->location_row[pos] >= '0' &&
               c->location_row[pos] <= '9') {
          size_t digit = (size_t)(c->location_row[pos++] - '0');
          if (values[field] > ((size_t)-1 - digit) / 10U)
            return cai_set_error(error, CAI_ERR_PROTOCOL,
                                 "patch hunk location overflows");
          values[field] = values[field] * 10U + digit;
        }
        if (pos == start || values[field] == 0U ||
            (field == 0U
                 ? pos == c->location_length || c->location_row[pos++] != ' '
                 : pos != c->location_length))
          return cai_set_error(error, CAI_ERR_PROTOCOL,
                               "invalid patch hunk locations");
      }
      location = cai_alloc(NULL, sizeof(*location));
      if (location == NULL)
        return cai_set_error(error, CAI_ERR_NOMEM,
                             "cannot retain patch hunk location");
      location->old_line = values[0];
      location->new_line = values[1];
      location->next = NULL;
      if (c->locations_tail != NULL)
        c->locations_tail->next = location;
      else
        c->locations = location;
      c->locations_tail = location;
      c->location_length = 0U;
    } else {
      if (c->location_length >= sizeof(c->location_row) - 1U ||
          (data[i] != ' ' && (data[i] < '0' || data[i] > '9')))
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "invalid patch hunk location bytes");
      c->location_row[c->location_length++] = data[i];
    }
  }
  if (n == 0U && c->location_length != 0U)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "truncated patch hunk location");
  if (n == 0U)
    c->locations_done = 1;
  return CAI_OK;
}
int cai_tool_display_set_width(cai_tool_display *d, size_t width,
                               cai_error *error) {
  if (d == NULL || (width != 0U && (width < 32U || width > 4096U)))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch width must be zero or 32..4096 columns");
  d->width = width;
  return CAI_OK;
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
  d->breaks = 2U;
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
int cai_tool_display_set_color(cai_tool_display *d, int enabled,
                               cai_error *error) {
  if (d == NULL || (enabled != 0 && enabled != 1))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool display color must be zero or one");
  d->config.color = enabled;
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
  if (e->type == CAI_AGENT_EVENT_REASONING_TOOL_RESULT)
    return display_marker(d, 1, "Reasoning", e->data != NULL ? e->data : "",
                          e->data_length, error);
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
  if (e->type == CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS) {
    if (c == NULL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "patch locations have no originating call");
    return patch_locations(c, e->data, e->data_length, error);
  }
  if (e->type == CAI_AGENT_EVENT_TOOL_PATCH) {
    if (c == NULL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "patch display has no originating call");
    if (c->location_length != 0U ||
        (c->locations != NULL && !c->locations_done))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "patch arrived before hunk locations completed");
    c->patch_started = 1;
    rc = patch_feed(d, c, e->data, e->data_length, e->data_length == 0U, error);
    if (rc == CAI_OK && e->data_length == 0U && c->locations != NULL)
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "patch has unused hunk locations");
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
    return c != NULL ? command_feed(d, c, e->data, e->data_length, 0, error)
                     : CAI_OK;
  if (e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED ||
      e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED) {
    char completed[80];
    if (c == NULL)
      return CAI_OK;
    if (!c->command_active)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "terminal completion has no started command");
    rc = command_feed(d, c, e->data, e->data_length, 1, error);
    if (rc == CAI_OK && c->partial_line)
      rc = display_text(d, "\n", error);
    if (rc == CAI_OK && e->terminal_output_truncated)
      rc = display_text(
          d, "  [command output truncated by the tool's output limit]\n",
          error);
    ok = e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED &&
         e->terminal_has_exit_code && e->terminal_exit_code == 0 &&
         (!e->terminal_has_signal || e->terminal_signal == 0);
    if (rc == CAI_OK && !c->replay) {
      display_command_verb(completed,
                           e->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED
                               ? "Cancelled"
                               : "Ran",
                           e->terminal_duration_ms);
      rc = display_marker(d, ok, completed, c->command, strlen(c->command),
                          error);
    }
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
  if (ok && e->tool_name != NULL &&
      strcmp(e->tool_name, "set_reasoning") == 0) {
    if (c != NULL)
      display_remove(d, c);
    return CAI_OK; /* The user receipt describes its actual outcome. */
  }
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
int cai_tool_display_summary(cai_tool_display *d,
                             const cai_agent_runtime_event *e, const char *verb,
                             const char *label, int success, cai_error *error) {
  cai_display_call *call;
  int rc;
  if (d == NULL || e == NULL || verb == NULL || label == NULL ||
      (e->type != CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
       e->type != CAI_AGENT_EVENT_TOOL_CALL_FAILED))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool summary requires a completion and plain labels");
  call = display_find(d, e);
  rc = display_marker(d, success != 0, verb, label, strlen(label), error);
  if (call != NULL)
    display_remove(d, call);
  return rc;
}

int cai_tool_display_replay_begin(cai_tool_display *d,
                                  const cai_agent_runtime_event *e,
                                  const char *verb, const char *label,
                                  int success, cai_error *error) {
  cai_display_call *call;
  char completed[80];
  int rc;
  if (d == NULL || e == NULL || verb == NULL || label == NULL ||
      (e->type != CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
       e->type != CAI_AGENT_EVENT_TOOL_CALL_FAILED))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "replay requires a completion and plain labels");
  call = display_find(d, e);
  if (call != NULL)
    display_remove(d, call);
  rc = display_call_new(d, e, &call, error);
  if (rc != CAI_OK)
    return rc;
  call->command_active = 1;
  call->replay = 1;
  if (strcmp(verb, "Ran") == 0) {
    display_command_verb(completed, verb, e->terminal_duration_ms);
    verb = completed;
  }
  return display_marker(d, success != 0, verb, label, strlen(label), error);
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
int cai_patch_render_width(cai_source *patch,
                           const cai_tool_display_config *config, size_t width,
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
  rc = cai_tool_display_set_width(d, width, error);
  if (rc != CAI_OK) {
    cai_tool_display_close(d);
    return rc;
  }
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
  while (c.locations != NULL) {
    patch_location *next = c.locations->next;
    cai_free_mem(NULL, c.locations);
    c.locations = next;
  }
  cai_free_mem(NULL, c.line);
  cai_free_mem(NULL, c.first);
  cai_free_mem(NULL, c.last);
  cai_tool_display_close(d);
  cai_error_cleanup(&local_error);
  return rc;
}

int cai_patch_render(cai_source *patch, const cai_tool_display_config *config,
                     cai_error *error) {
  return cai_patch_render_width(patch, config, 0U, error);
}
