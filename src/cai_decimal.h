#ifndef CAI_DECIMAL_H
#define CAI_DECIMAL_H

#include <limits.h>
#include <stddef.h>

/* strtoull() is not declared by every supported SDK in strict C89 mode. */
static int cai_parse_unsigned_decimal(const char *value, const char **end_out,
                                      unsigned long long *result_out) {
  const unsigned char *cursor;
  unsigned long long result;
  unsigned long long digit;

  if (value == NULL || end_out == NULL || result_out == NULL) {
    return -1;
  }
  cursor = (const unsigned char *)value;
  if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
    return -1;
  }
  result = 0U;
  do {
    digit = (unsigned long long)(*cursor - (unsigned char)'0');
    if (result > (ULLONG_MAX - digit) / 10U) {
      return -1;
    }
    result = result * 10U + digit;
    cursor++;
  } while (*cursor >= (unsigned char)'0' && *cursor <= (unsigned char)'9');
  *end_out = (const char *)cursor;
  *result_out = result;
  return 0;
}

#endif
