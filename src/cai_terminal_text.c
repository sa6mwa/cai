#include "cai_terminal_text.h"
#include "cai_internal.h"
#include <string.h>

enum { TEXT, ESCAPE, INTERMEDIATE, CSI, STRING, STRING_ESCAPE };

void cai_terminal_text_close(cai_terminal_text *t) {
  if (t == NULL)
    return;
  cai_free_mem(NULL, t->rows);
  cai_free_mem(NULL, t->line_storage);
  memset(t, 0, sizeof(*t));
}

int cai_terminal_text_init(cai_terminal_text *t, size_t lines, size_t bytes,
                           cai_error *error) {
  memset(t, 0, sizeof(*t));
  if (lines == 0U || bytes < 4U || bytes > (size_t)-1 / 2U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "shell capture requires positive lines and >=4 bytes");
  t->rows = cai_alloc(NULL, bytes);
  t->line_storage = cai_alloc(NULL, bytes * 2U + 1U);
  t->line = t->line_storage;
  if (t->rows == NULL || t->line == NULL) {
    cai_terminal_text_close(t);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "cannot allocate bounded shell capture");
  }
  t->line_limit = lines;
  t->byte_limit = bytes;
  return CAI_OK;
}

static void discard_row(cai_terminal_text *t) {
  char ch;
  do {
    ch = t->rows[t->start];
    t->start = (t->start + 1U) % t->byte_limit;
    t->length--;
  } while (t->length != 0U && ch != '\n');
  if (t->lines != 0U)
    t->lines--;
  t->truncated = 1;
}

static void discard_bytes(cai_terminal_text *t, size_t count) {
  while (t->length != 0U &&
         (count != 0U || ((unsigned char)t->rows[t->start] & 0xc0U) == 0x80U)) {
    if (t->rows[t->start] == '\n')
      t->lines--;
    t->start = (t->start + 1U) % t->byte_limit;
    t->length--;
    if (count != 0U)
      count--;
    t->truncated = 1;
  }
}

static size_t scalar_end(const char *line, size_t at, size_t length) {
  if (at < length)
    at++;
  while (at < length && ((unsigned char)line[at] & 192U) == 128U)
    at++;
  return at;
}

static void put_scalar(cai_terminal_text *t, const char *data, size_t size) {
  size_t end = scalar_end(t->line, t->cursor, t->line_length);
  size_t next = t->line_length - (end - t->cursor) + size;
  while (t->lines != 0U && t->lines >= t->line_limit)
    discard_row(t);
  if (t->length + next > t->byte_limit)
    discard_bytes(t, t->length + next - t->byte_limit);
  /* One oversized logical line retains its UTF-8 suffix, also under the bound.
   */
  while (next > t->byte_limit) {
    size_t drop = next - t->byte_limit;
    if (drop > t->line_length)
      drop = t->line_length;
    while (drop < t->line_length &&
           ((unsigned char)t->line[drop] & 192U) == 128U)
      drop++;
    if (drop == 0U)
      drop = scalar_end(t->line, 0U, t->line_length);
    t->line += drop;
    t->line_length -= drop;
    t->cursor = t->cursor >= drop ? t->cursor - drop : 0U;
    end = scalar_end(t->line, t->cursor, t->line_length);
    next = t->line_length - (end - t->cursor) + size;
    t->truncated = 1;
  }
  /* Sliding storage avoids shifting a full line on every appended byte while
   * retaining the exact newest suffix, including an unterminated long line. */
  if ((size_t)(t->line - t->line_storage) + next > t->byte_limit * 2U) {
    memmove(t->line_storage, t->line, t->line_length);
    t->line = t->line_storage;
  }
  memmove(t->line + t->cursor + size, t->line + end, t->line_length - end);
  memcpy(t->line + t->cursor, data, size);
  t->line_length = next;
  t->cursor += size;
}

static void newline(cai_terminal_text *t) {
  size_t i;
  while (t->lines != 0U && t->lines >= t->line_limit)
    discard_row(t);
  if (t->length + t->line_length + 1U > t->byte_limit)
    discard_bytes(t, t->length + t->line_length + 1U - t->byte_limit);
  if (t->line_length == t->byte_limit) {
    size_t drop = scalar_end(t->line, 0U, t->line_length);
    t->line += drop;
    t->line_length -= drop;
    t->truncated = 1;
  }
  for (i = 0U; i < t->line_length; i++)
    t->rows[(t->start + t->length++) % t->byte_limit] = t->line[i];
  t->rows[(t->start + t->length++) % t->byte_limit] = '\n';
  t->lines++;
  t->line_length = t->cursor = 0U;
  t->line = t->line_storage;
}

static void csi_finish(cai_terminal_text *t, unsigned char ch) {
  size_t n = t->parameter_seen ? t->parameter : 1U;
  if (ch == 'K') {
    if (!t->parameter_seen || t->parameter == 0U)
      t->line_length = t->cursor;
    else if (t->parameter == 1U)
      memset(t->line, ' ', t->cursor);
    else if (t->parameter == 2U)
      t->line_length = t->cursor = 0U;
  } else if (ch == 'D' || ch == 'C') {
    while (n-- != 0U &&
           (ch == 'D' ? t->cursor != 0U : t->cursor < t->line_length)) {
      if (ch == 'D') {
        if (t->cursor != 0U) {
          t->cursor--;
          while (t->cursor != 0U &&
                 ((unsigned char)t->line[t->cursor] & 192U) == 128U)
            t->cursor--;
        }
      } else if (t->cursor < t->line_length)
        t->cursor = scalar_end(t->line, t->cursor, t->line_length);
    }
  } else if (ch == 'G' || ch == '`') {
    t->cursor = 0U;
    while (n > 1U && t->cursor < t->line_length) {
      t->cursor = scalar_end(t->line, t->cursor, t->line_length);
      n--;
    }
  }
}

static void ascii(cai_terminal_text *t, unsigned char ch) {
  if (ch == 27U) {
    t->state = ESCAPE;
  } else if (ch == 155U) {
    t->state = CSI;
    t->parameter = 0U;
    t->parameter_seen = 0;
  } else if (ch == 144U || ch == 152U || ch == 157U || ch == 158U ||
             ch == 159U) {
    t->state = STRING;
    t->string_kind = ch == 157U;
  } else if (ch == '\n' || ch == 133U)
    newline(t);
  else if (ch == '\r')
    t->cursor = 0U;
  else if (ch == '\b') {
    if (t->cursor != 0U) {
      t->cursor--;
      while (t->cursor != 0U &&
             ((unsigned char)t->line[t->cursor] & 192U) == 128U)
        t->cursor--;
    }
  } else if (ch == '\t' || (ch >= 32U && ch < 127U)) {
    char byte = (char)ch;
    put_scalar(t, &byte, 1U);
  }
}

static void byte(cai_terminal_text *t, unsigned char ch) {
  static const char replacement[] = "\xef\xbf\xbd";
  unsigned long scalar;
  size_t i;
  if (t->state == STRING || t->state == STRING_ESCAPE) {
    if (ch == 156U || (t->string_kind && ch == 7U) ||
        (t->state == STRING_ESCAPE && ch == '\\'))
      t->state = TEXT;
    else if (ch == 24U || ch == 26U)
      t->state = TEXT;
    else
      t->state = ch == 27U ? STRING_ESCAPE : STRING;
    return;
  }
  if (t->state == ESCAPE) {
    if (ch == '[') {
      t->state = CSI;
      t->parameter = 0U;
      t->parameter_seen = 0;
    } else if (ch == ']' || ch == 'P' || ch == 'X' || ch == '^' || ch == '_') {
      t->state = STRING;
      t->string_kind = ch == ']';
    } else if (ch >= 32U && ch <= 47U)
      t->state = INTERMEDIATE;
    else if (ch != 27U) {
      t->state = TEXT;
      if (ch == 'D' || ch == 'E')
        newline(t);
    }
    return;
  }
  if (t->state == INTERMEDIATE || t->state == CSI) {
    if (ch == 27U) {
      t->state = ESCAPE;
      return;
    }
    if (ch == 24U || ch == 26U) {
      t->state = TEXT;
      return;
    }
    if (t->state == CSI && ch >= '0' && ch <= '9') {
      if (t->parameter < t->byte_limit / 10U)
        t->parameter = t->parameter * 10U + (unsigned int)(ch - '0');
      else
        t->parameter = t->byte_limit;
      t->parameter_seen = 1;
    } else if (ch >= 64U && ch <= 126U) {
      if (t->state == CSI)
        csi_finish(t, ch);
      t->state = TEXT;
    } else if (t->state == INTERMEDIATE && ch >= 48U && ch <= 126U)
      t->state = TEXT;
    return;
  }
  if (t->utf8_length != 0U) {
    if ((ch & 192U) != 128U) {
      t->utf8_length = 0U;
      put_scalar(t, replacement, 3U);
      byte(t, ch);
      return;
    }
    t->utf8[t->utf8_length++] = ch;
    if (t->utf8_length != t->utf8_need)
      return;
    scalar = t->utf8[0] & ((1U << (7U - t->utf8_need)) - 1U);
    for (i = 1U; i < t->utf8_need; i++)
      scalar = (scalar << 6) | (t->utf8[i] & 63U);
    if ((t->utf8_need == 2U && scalar < 128UL) ||
        (t->utf8_need == 3U && scalar < 2048UL) ||
        (t->utf8_need == 4U && scalar < 65536UL) || scalar > 1114111UL ||
        (scalar >= 55296UL && scalar <= 57343UL))
      put_scalar(t, replacement, 3U);
    else if (scalar >= 128UL && scalar <= 159UL)
      ascii(t, (unsigned char)scalar);
    else
      put_scalar(t, (const char *)t->utf8, t->utf8_need);
    t->utf8_length = 0U;
    return;
  }
  if (ch < 160U)
    ascii(t, ch);
  else if (ch >= 194U && ch <= 244U) {
    t->utf8[0] = ch;
    t->utf8_length = 1U;
    t->utf8_need = ch < 224U ? 2U : ch < 240U ? 3U : 4U;
  } else
    put_scalar(t, replacement, 3U);
}

void cai_terminal_text_write(cai_terminal_text *t, const void *bytes,
                             size_t length) {
  const unsigned char *data = bytes;
  size_t i;
  for (i = 0U; i < length; i++)
    byte(t, data[i]);
}

int cai_terminal_text_finish(cai_terminal_text *t, char **out,
                             cai_error *error) {
  size_t i;
  char *copy;
  if (t->utf8_length != 0U) {
    put_scalar(t, "\xef\xbf\xbd", 3U);
    t->utf8_length = 0U;
  }
  copy = cai_alloc(NULL, t->length + t->line_length + 1U);
  if (copy == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "cannot copy shell capture");
  for (i = 0U; i < t->length; i++)
    copy[i] = t->rows[(t->start + i) % t->byte_limit];
  memcpy(copy + t->length, t->line, t->line_length);
  copy[t->length + t->line_length] = '\0';
  *out = copy;
  return CAI_OK;
}
