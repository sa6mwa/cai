#include "../cli/review_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char report[] =
      "{\"findings\":[{\"title\":\"Fix parser\",\"body\":\"Reject bad input\","
      "\"confidence_score\":0.9,\"priority\":1,\"code_location\":{"
      "\"absolute_file_path\":\"/tmp/work/parser.c\",\"line_range\":{"
      "\"start\":4,\"end\":6}}}],\"overall_correctness\":\"patch is "
      "incorrect\",\"overall_explanation\":\"One issue found\","
      "\"overall_confidence_score\":0.9}";
  const char clean[] =
      "{\"findings\":[],\"overall_correctness\":\"patch is correct\","
      "\"overall_explanation\":\"No issues\","
      "\"overall_confidence_score\":0.9}";
  char *formatted = NULL;
  if (cai_cli_review_format(report, strlen(report), "markdown", &formatted) !=
          0 ||
      formatted == NULL || strstr(formatted, "# Review findings") == NULL ||
      strstr(formatted, "## Fix parser") == NULL ||
      strstr(formatted, "Priority: P1") == NULL ||
      strstr(formatted, "/tmp/work/parser.c:4-6") == NULL ||
      strstr(formatted, "Reject bad input") == NULL) {
    fputs("review Markdown formatting failed\n", stderr);
    free(formatted);
    return 1;
  }
  free(formatted);
  formatted = NULL;
  if (cai_cli_review_format(report, strlen(report), "json", &formatted) != 0 ||
      formatted == NULL || strncmp(formatted, report, strlen(report)) != 0 ||
      formatted[strlen(report)] != '\n') {
    fputs("review JSON formatting failed\n", stderr);
    free(formatted);
    return 1;
  }
  free(formatted);
  formatted = NULL;
  if (cai_cli_review_format(clean, strlen(clean), "markdown", &formatted) !=
          0 ||
      formatted == NULL ||
      strstr(formatted, "No actionable findings.") == NULL) {
    fputs("clean review formatting failed\n", stderr);
    free(formatted);
    return 1;
  }
  free(formatted);
  return 0;
}
