#include "../src/cai_terminal_text.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *input, size_t length, const char *expected) {
  cai_terminal_text t;
  cai_error error;
  char *out;
  size_t split;
  cai_error_init(&error);
  for (split = 0U; split <= length; split++) {
    assert(cai_terminal_text_init(&t, 20U, 4096U, &error) == CAI_OK);
    cai_terminal_text_write(&t, input, split);
    cai_terminal_text_write(&t, input + split, length - split);
    assert(cai_terminal_text_finish(&t, &out, &error) == CAI_OK);
    if (strcmp(out, expected) != 0) {
      fprintf(stderr, "split %lu: expected [%s], got [%s]\n",
              (unsigned long)split, expected, out);
      abort();
    }
    free(out);
    cai_terminal_text_close(&t);
  }
  cai_error_cleanup(&error);
}
#define CHECK(input, expected) check(input, sizeof(input) - 1U, expected)

int main(void) {
  cai_terminal_text t;
  cai_error error;
  char *out;
  char line[40];
  size_t i;
  CHECK("plain\r\n\033[31mred\033[0m\t€🚀", "plain\nred\t€🚀");
  CHECK("\033]0;secret\007visible\033]8;;https://hidden\033\\link"
        "\033]8;;\033\\\033Psecret\033\\\033Xsecret\033\\"
        "\033^secret\033\\\033_secret\033\\\033(B!",
        "visiblelink!");
  CHECK("\x9dhidden\x9c"
        "C1\xc2\x9b"
        "31mred\xc2\x9b"
        "0m",
        "C1red");
  CHECK("long old text\rnew\033[K\nabc\bX\nabc\033[2DX\n"
        "abcdef\033[2GZ\nold\r\033[2Kdone",
        "new\nabX\naXc\naZcdef\ndone");
  CHECK("a\0\007\177\377\342", "a��");
  CHECK("\xe2X\xe0\x80\x80\xed\xa0\x80", "�X��");
  CHECK("keep\033[31", "keep");
  CHECK("keep\033]unfinished", "keep");
  CHECK("\033]0;\xc3\x9c"
        "hidden title\007visible",
        "visible");
  CHECK("\033P\xe2\x9c\x93"
        "hidden DCS\033\\visible",
        "visible");
  CHECK("\033_\xf0\x9f\x9c\x80"
        "hidden APC\033\\visible",
        "visible");
  CHECK("\x9d\xc3\x9c"
        "hidden\x9cvisible",
        "visible");
  CHECK("\033]hidden\xc2\x9cvisible", "visible");
  CHECK("\033]hidden\033\xc3\x9c\\still hidden\007visible", "visible");
  CHECK("\033]hidden\xe0\x80\x9c"
        "still hidden\007visible",
        "visible");
  CHECK("\033]hidden\xe2\007visible", "visible");
  CHECK("visible\033]hidden\xe2\x9c", "visible");
  CHECK("a\033[999999999999999999999D"
        "b",
        "b");
  cai_error_init(&error);
  assert(cai_terminal_text_init(&t, 5000U, 1024U * 1024U, &error) == CAI_OK);
  for (i = 0U; i < 6000U; i++) {
    snprintf(line, sizeof(line), "row-%04lu\n", (unsigned long)i);
    cai_terminal_text_write(&t, line, strlen(line));
  }
  assert(cai_terminal_text_finish(&t, &out, &error) == CAI_OK);
  assert(t.truncated && t.lines == 5000U);
  assert(strncmp(out, "row-1000\n", 9U) == 0);
  assert(strstr(out, "row-5999\n") != NULL);
  free(out);
  cai_terminal_text_write(&t, "partial", 7U);
  assert(cai_terminal_text_finish(&t, &out, &error) == CAI_OK);
  assert(strncmp(out, "row-1001\n", 9U) == 0);
  assert(strcmp(out + strlen(out) - 7U, "partial") == 0);
  free(out);
  cai_terminal_text_close(&t);
  assert(cai_terminal_text_init(&t, 2U, 8U, &error) == CAI_OK);
  cai_terminal_text_write(&t, "old\n", 4U);
  for (i = 0U; i < 1000U; i++)
    cai_terminal_text_write(&t, "€", 3U);
  assert(cai_terminal_text_finish(&t, &out, &error) == CAI_OK);
  assert(strlen(out) <= 8U && strcmp(out, "€€") == 0 && t.truncated);
  free(out);
  cai_terminal_text_close(&t);
  /* A byte ceiling retains an exact suffix, even part of the oldest row. */
  assert(cai_terminal_text_init(&t, 20U, 8U, &error) == CAI_OK);
  cai_terminal_text_write(&t, "abcde\nfgh\n", 10U);
  assert(cai_terminal_text_finish(&t, &out, &error) == CAI_OK);
  assert(strcmp(out, "cde\nfgh\n") == 0 && t.truncated);
  free(out);
  cai_terminal_text_close(&t);
  assert(cai_terminal_text_init(&t, 20U, 17U, &error) == CAI_OK);
  for (i = 0U; i < 100000U; i++) {
    char byte = (char)('a' + i % 26U);
    cai_terminal_text_write(&t, &byte, 1U);
  }
  assert(cai_terminal_text_finish(&t, &out, &error) == CAI_OK);
  assert(strlen(out) == 17U && t.truncated);
  for (i = 0U; i < 17U; i++)
    assert(out[i] == (char)('a' + (100000U - 17U + i) % 26U));
  free(out);
  cai_terminal_text_close(&t);
  assert(cai_terminal_text_init(&t, 0U, 8U, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  return 0;
}
