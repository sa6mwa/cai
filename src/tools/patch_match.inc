/* Codex-compatible line matching. Plan against the original source once;
 * preserve context bytes and publish only after the whole plan validates. */
typedef struct cai_patch_line {
  const char *text;
  size_t length, ending;
} cai_patch_line;

static int cai_patch_split_lines(const char *text, size_t length,
                                 cai_patch_line **out, size_t *count,
                                 cai_error *error) {
  size_t i, start, n = 0U;
  cai_patch_line *lines;
  *out = NULL;
  *count = 0U;
  for (i = 0U; i < length; i++) {
    if (text[i] == '\r' || text[i] == '\n') {
      n++;
      if (text[i] == '\r' && i + 1U < length && text[i + 1U] == '\n')
        i++;
    }
  }
  if (length != 0U && text[length - 1U] != '\n' && text[length - 1U] != '\r')
    n++;
  if (n == 0U)
    return CAI_OK;
  if (n > SIZE_MAX / sizeof(*lines))
    return cai_set_error(error, CAI_ERR_LIMIT, "patch line index is too large");
  lines = cai_alloc(NULL, n * sizeof(*lines));
  if (lines == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate patch line index");
  start = n = 0U;
  for (i = 0U; i < length; i++) {
    if (text[i] != '\r' && text[i] != '\n')
      continue;
    lines[n].text = text + start;
    lines[n].length = i - start;
    lines[n].ending =
        text[i] == '\r' && i + 1U < length && text[i + 1U] == '\n' ? 2U : 1U;
    i += lines[n++].ending - 1U;
    start = i + 1U;
  }
  if (start < length) {
    lines[n].text = text + start;
    lines[n].length = length - start;
    lines[n++].ending = 0U;
  }
  *out = lines;
  *count = n;
  return CAI_OK;
}

/* Invalid UTF-8 remains literal and cannot normalize into ASCII punctuation. */
static unsigned long cai_patch_codepoint(const char **cursor, const char *end) {
  const unsigned char *p = (const unsigned char *)*cursor;
  unsigned long value;
  size_t n, i;
  unsigned int ch = *p;
  if (ch < 128U) {
    (*cursor)++;
    return ch;
  }
  n = ch >= 194U && ch <= 223U   ? 2U
      : ch >= 224U && ch <= 239U ? 3U
      : ch >= 240U && ch <= 244U ? 4U
                                 : 0U;
  if (n == 0U || (size_t)(end - *cursor) < n) {
    (*cursor)++;
    return 0x110000UL + ch;
  }
  value = ch & (n == 2U ? 31U : n == 3U ? 15U : 7U);
  for (i = 1U; i < n; i++) {
    if ((p[i] & 192U) != 128U) {
      (*cursor)++;
      return 0x110000UL + ch;
    }
    value = (value << 6U) | (p[i] & 63U);
  }
  if ((n == 3U && value < 0x800UL) || (n == 4U && value < 0x10000UL) ||
      (value >= 0xd800UL && value <= 0xdfffUL) || value > 0x10ffffUL) {
    (*cursor)++;
    return 0x110000UL + ch;
  }
  *cursor += n;
  return value;
}

static int cai_patch_whitespace(unsigned long ch) {
  return (ch >= 9UL && ch <= 13UL) || ch == 32UL || ch == 0x85UL ||
         ch == 0xa0UL || ch == 0x1680UL || (ch >= 0x2000UL && ch <= 0x200aUL) ||
         ch == 0x2028UL || ch == 0x2029UL || ch == 0x202fUL || ch == 0x205fUL ||
         ch == 0x3000UL;
}

static unsigned long cai_patch_normalize_character(unsigned long ch) {
  if ((ch >= 0x2010UL && ch <= 0x2015UL) || ch == 0x2212UL)
    return '-';
  if (ch >= 0x2018UL && ch <= 0x201bUL)
    return '\'';
  if (ch >= 0x201cUL && ch <= 0x201fUL)
    return '"';
  if (ch == 0xa0UL || (ch >= 0x2002UL && ch <= 0x200aUL) || ch == 0x202fUL ||
      ch == 0x205fUL || ch == 0x3000UL)
    return ' ';
  return ch;
}

static void cai_patch_trim(cai_patch_line *line, int leading) {
  const char *cursor = line->text, *end = cursor + line->length;
  const char *first = NULL, *last = cursor, *at;
  while (cursor < end) {
    at = cursor;
    if (!cai_patch_whitespace(cai_patch_codepoint(&cursor, end))) {
      if (first == NULL)
        first = at;
      last = cursor;
    }
  }
  if (leading)
    line->text = first != NULL ? first : end;
  line->length = last >= line->text ? (size_t)(last - line->text) : 0U;
}

static int cai_patch_line_equal(cai_patch_line a, cai_patch_line b, int pass) {
  const char *x, *y, *xe, *ye;
  if (pass != 0) {
    cai_patch_trim(&a, pass >= 2);
    cai_patch_trim(&b, pass >= 2);
  }
  if (pass < 3)
    return a.length == b.length && memcmp(a.text, b.text, a.length) == 0;
  x = a.text;
  xe = x + a.length;
  y = b.text;
  ye = y + b.length;
  while (x < xe && y < ye)
    if (cai_patch_normalize_character(cai_patch_codepoint(&x, xe)) !=
        cai_patch_normalize_character(cai_patch_codepoint(&y, ye)))
      return 0;
  return x == xe && y == ye;
}

/* Search all exact candidates before relaxing whitespace, just like Codex.
 * Repeated context selects the first occurrence at/after the source cursor. */
static int cai_patch_seek(const cai_patch_line *lines, size_t count,
                          const cai_patch_line *pattern, size_t length,
                          size_t start, int eof, size_t *found) {
  size_t i, j;
  int pass;
  if (length == 0U) {
    if (start > count)
      return 0;
    *found = start;
    return 1;
  }
  if (length > count || start > count - length)
    return 0;
  if (eof && count - length > start)
    start = count - length;
  for (pass = 0; pass < 4; pass++) {
    for (i = start; i <= count - length; i++) {
      for (j = 0U; j < length; j++)
        if (!cai_patch_line_equal(lines[i + j], pattern[j], pass))
          break;
      if (j == length) {
        *found = i;
        return 1;
      }
    }
  }
  return 0;
}

static int cai_patch_hunk_order(const void *a, const void *b) {
  const cai_patch_hunk *x = *(cai_patch_hunk *const *)a;
  const cai_patch_hunk *y = *(cai_patch_hunk *const *)b;
  if (x->old_line != y->old_line)
    return x->old_line < y->old_line ? -1 : 1;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int cai_patch_emit_line(cai_buffer_builder *output, cai_patch_line line,
                               const char *ending, size_t ending_length,
                               size_t limit, cai_error *error) {
  size_t length;
  int rc;
  if (!cai_patch_size_add(line.length,
                          line.ending != 0U ? line.ending : ending_length,
                          &length) ||
      output->length > limit || length > limit - output->length)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patched file exceeds size limit");
  rc = cai_buffer_append(output, line.text, line.length, error);
  if (rc == CAI_OK)
    rc = cai_buffer_append(
        output, line.ending != 0U ? line.text + line.length : ending,
        line.ending != 0U ? line.ending : ending_length, error);
  return rc;
}

static int cai_patch_apply_hunks(cai_patch_change *change, size_t limit,
                                 cai_error *error) {
  cai_patch_line *source = NULL, *old = NULL, *added = NULL;
  cai_patch_hunk **ordered = NULL;
  cai_buffer_builder output;
  const char *ending = "\n", *search;
  size_t count = 0U, old_count = 0U, new_count = 0U, ending_length = 1U;
  size_t i, j, cursor = 0U, at, found, old_index, new_index, output_line = 1U;
  int rc;
  memset(&output, 0, sizeof(output));
  rc = cai_patch_split_lines(change->before, change->before_length, &source,
                             &count, error);
  if (rc != CAI_OK)
    goto done;
  if (count != 0U && source[0].ending != 0U) {
    ending = source[0].text + source[0].length;
    ending_length = source[0].ending;
  }
  ordered = cai_alloc(NULL, change->hunk_count * sizeof(*ordered));
  if (change->hunk_count != 0U && ordered == NULL) {
    rc =
        cai_set_error(error, CAI_ERR_NOMEM, "allocate patch replacement order");
    goto done;
  }
  for (i = 0U; i < change->hunk_count; i++) {
    cai_patch_hunk *hunk = &change->hunks[i];
    ordered[i] = hunk;
    if (hunk->operations.length == 0U) {
      rc = cai_set_error(error, CAI_ERR_INVALID, "update hunk is empty");
      goto done;
    }
    if (hunk->context != NULL && hunk->context[0] != '\0') {
      cai_patch_line anchor;
      anchor.text = hunk->context;
      anchor.length = strlen(anchor.text);
      anchor.ending = 0U;
      if (!cai_patch_seek(source, count, &anchor, 1U, cursor, 0, &found)) {
        rc = cai_patch_hunk_error(change, hunk, "anchor was not found",
                                  change->before, 0, error);
        goto done;
      }
      cursor = found + 1U;
    }
    rc = cai_patch_split_lines(hunk->old_text, hunk->old_length, &old,
                               &old_count, error);
    if (rc != CAI_OK)
      goto done;
    if (old_count == 0U)
      found = count;
    else if (!cai_patch_seek(source, count, old, old_count, cursor,
                             hunk->end_of_file, &found)) {
      if (old[old_count - 1U].length != 0U ||
          !cai_patch_seek(source, count, old, old_count - 1U, cursor,
                          hunk->end_of_file, &found)) {
        search = cursor < count ? source[cursor].text
                                : change->before + change->before_length;
        rc = cai_patch_hunk_error(change, hunk,
                                  hunk->end_of_file
                                      ? "does not match the end of file"
                                      : "does not match",
                                  search, 1, error);
        goto done;
      }
      old_count--;
      hunk->trim_final_blank = 1;
    }
    hunk->old_line = found + 1U;
    hunk->match_count = old_count;
    if (hunk->old_length != 0U)
      cursor = found + old_count;
    cai_free_mem(NULL, old);
    old = NULL;
  }
  if (change->hunk_count > 1U)
    qsort(ordered, change->hunk_count, sizeof(*ordered), cai_patch_hunk_order);
  cursor = 0U;
  for (i = 0U; i < change->hunk_count; i++) {
    cai_patch_hunk *hunk = ordered[i];
    at = hunk->old_line - 1U;
    if (at < cursor) {
      rc = cai_set_error(error, CAI_ERR_INVALID, "patch hunks overlap");
      goto done;
    }
    while (cursor < at && rc == CAI_OK) {
      rc = cai_patch_emit_line(&output, source[cursor++], ending, ending_length,
                               limit, error);
      output_line++;
    }
    hunk->new_line = output_line;
    if (rc == CAI_OK)
      rc = cai_patch_split_lines(hunk->new_text, hunk->new_length, &added,
                                 &new_count, error);
    if (rc != CAI_OK)
      goto done;
    if (hunk->trim_final_blank && new_count != 0U &&
        added[new_count - 1U].length == 0U)
      new_count--;
    old_index = new_index = 0U;
    for (j = 0U; j < hunk->operations.length && rc == CAI_OK; j++) {
      char kind = hunk->operations.data[j];
      if (kind != '-' && new_index < new_count) {
        cai_patch_line line = added[new_index];
        if (kind == ' ' && old_index < hunk->match_count)
          line = source[at + old_index];
        else
          line.ending = 0U;
        rc = cai_patch_emit_line(&output, line, ending, ending_length, limit,
                                 error);
        output_line++;
      }
      if (kind != '+')
        old_index++;
      if (kind != '-')
        new_index++;
    }
    cursor = at + hunk->match_count;
    hunk->emitted_lines = output_line - hunk->new_line;
    cai_free_mem(NULL, added);
    added = NULL;
  }
  while (cursor < count && rc == CAI_OK)
    rc = cai_patch_emit_line(&output, source[cursor++], ending, ending_length,
                             limit, error);
  if (rc == CAI_OK) {
    if (output.data == NULL)
      rc = cai_buffer_append(&output, "", 0U, error);
    if (rc == CAI_OK) {
      change->after = output.data;
      change->after_length = output.length;
      output.data = NULL;
    }
  }
done:
  cai_free_mem(NULL, source);
  cai_free_mem(NULL, old);
  cai_free_mem(NULL, added);
  cai_free_mem(NULL, ordered);
  cai_free_mem(NULL, output.data);
  return rc;
}
