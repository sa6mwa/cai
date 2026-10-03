#include "literal.h"
#include "../src/cai_utf8.h"
#include <string.h>

static int literal_token(mdf *renderer, int kind, const char *data,
                         size_t length) {
  mdf_token token;
  memset(&token, 0, sizeof(token));
  token.type = (mdf_token_type)kind;
  token.text = data;
  token.len = length;
  return renderer->write_token(renderer, &token) == MDF_OK ? 0 : -1;
}
int cai_cli_literal_render(mdf *renderer, const char *text) {
  char chunk[256];
  size_t used = 0U;
  size_t offset;
  size_t length = strlen(text);
  for (offset = 0U; offset < length; offset++) {
    unsigned char ch = (unsigned char)text[offset];
    if (ch >= 128U || ch == '\n' || used + 5U > sizeof(chunk)) {
      if (used != 0U &&
          literal_token(renderer, MDF_TOKEN_TEXT, chunk, used) != 0)
        return -1;
      used = 0U;
    }
    if (ch == '\n') {
      if (literal_token(renderer, MDF_TOKEN_NEWLINE, NULL, 0U) != 0)
        return -1;
    } else if (ch >= 128U) {
      char rune[4];
      size_t need = ch >= 0xc2U && ch < 0xe0U    ? 2U
                    : ch >= 0xe0U && ch < 0xf0U  ? 3U
                    : ch >= 0xf0U && ch <= 0xf4U ? 4U
                                                 : 1U;
      if (need > length - offset)
        need = length - offset;
      memcpy(rune, text + offset, need);
      cai_utf8_repair_text(rune, need);
      if (rune[0] == '?')
        need = 1U;
      /* Keep UTF-8 separate from escape pairs: ANSI OFF may filter tokens
       * into smaller writes, while inline escapes require one token. */
      if (literal_token(renderer, MDF_TOKEN_TEXT, rune, need) != 0)
        return -1;
      offset += need - 1U;
    } else if (ch == '&') {
      memcpy(chunk + used, "&amp;", 5U);
      used += 5U;
    } else {
      /* TEXT tokens bypass block parsing but still recognize inline markup. */
      if (strchr("\\`*_{}[]()#+-.!<>|", (int)ch) != NULL)
        chunk[used++] = '\\';
      chunk[used++] = ch < 32U || ch == 127U ? ' ' : (char)ch;
    }
  }
  if (used != 0U && literal_token(renderer, MDF_TOKEN_TEXT, chunk, used) != 0)
    return -1;
  return renderer->finish(renderer) == MDF_OK ? 0 : -1;
}
