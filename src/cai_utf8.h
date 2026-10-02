#ifndef CAI_UTF8_H
#define CAI_UTF8_H
#include <stddef.h>

/* Repair a bounded, already-buffered text chunk in place for JSON strings.
 * Preserve valid UTF-8 exactly; invalid and incomplete bytes become '?'.
 * This is private shared byte handling, with no allocation or I/O. */
static void cai_utf8_repair_text(char *text, size_t length) {
  size_t i = 0U;
  while (i < length) {
    unsigned char lead = (unsigned char)text[i];
    size_t need, j;
    int valid;
    if (lead < 128U) {
      i++;
      continue;
    }
    need = lead >= 0xc2U && lead < 0xe0U    ? 2U
           : lead >= 0xe0U && lead < 0xf0U  ? 3U
           : lead >= 0xf0U && lead <= 0xf4U ? 4U
                                            : 0U;
    valid = need != 0U && need <= length - i;
    for (j = 1U; valid && j < need; j++)
      valid = ((unsigned char)text[i + j] & 0xc0U) == 0x80U;
    if (valid) {
      unsigned char second = (unsigned char)text[i + 1U];
      valid = !(lead == 0xe0U && second < 0xa0U) &&
              !(lead == 0xedU && second >= 0xa0U) &&
              !(lead == 0xf0U && second < 0x90U) &&
              !(lead == 0xf4U && second >= 0x90U);
    }
    if (valid)
      i += need;
    else {
      text[i] = '?';
      i++;
    }
  }
}
#endif
