#define _POSIX_C_SOURCE 200809L

#include "review_report.h"

#include <lonejson.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct review_line_range {
  lonejson_int64 start;
  lonejson_int64 end;
} review_line_range;

typedef struct review_code_location {
  char *absolute_file_path;
  review_line_range line_range;
} review_code_location;

typedef struct review_finding {
  char *title;
  char *body;
  double confidence_score;
  lonejson_int64 priority;
  int priority_present;
  review_code_location code_location;
} review_finding;

typedef struct review_report {
  lonejson_object_array findings;
  char *overall_correctness;
  char *overall_explanation;
  double overall_confidence_score;
} review_report;

static const lonejson_field range_fields[] = {
    LONEJSON_FIELD_I64_REQ(review_line_range, start, "start"),
    LONEJSON_FIELD_I64_REQ(review_line_range, end, "end")};
LONEJSON_MAP_DEFINE(range_map, review_line_range, range_fields);

static const lonejson_field location_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_code_location, absolute_file_path,
                                    "absolute_file_path"),
    LONEJSON_FIELD_OBJECT_REQ(review_code_location, line_range, "line_range",
                              &range_map)};
LONEJSON_MAP_DEFINE(location_map, review_code_location, location_fields);

static const lonejson_field finding_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_finding, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_finding, body, "body"),
    LONEJSON_FIELD_F64_REQ(review_finding, confidence_score,
                           "confidence_score"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(review_finding, priority,
                                        priority_present, "priority"),
    LONEJSON_FIELD_OBJECT_REQ(review_finding, code_location, "code_location",
                              &location_map)};
LONEJSON_MAP_DEFINE(finding_map, review_finding, finding_fields);

static const lonejson_field report_fields[] = {
    {"findings", sizeof("findings") - 1U, (unsigned char)'f',
     (unsigned char)'s', offsetof(review_report, findings),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(review_finding), &finding_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_report, overall_correctness,
                                    "overall_correctness"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_report, overall_explanation,
                                    "overall_explanation"),
    LONEJSON_FIELD_F64_REQ(review_report, overall_confidence_score,
                           "overall_confidence_score")};
LONEJSON_MAP_DEFINE(report_map, review_report, report_fields);

static void review_safe_text(FILE *stream, const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  while (*cursor != '\0') {
    if (*cursor == '\n' || *cursor == '\t') {
      fputc((int)*cursor, stream);
    } else if (*cursor < 32U || *cursor == 127U) {
      fputc('?', stream);
    } else if (*cursor == 0xc2U && cursor[1] >= 0x80U && cursor[1] <= 0x9fU) {
      fputc('?', stream);
      cursor++;
    } else {
      fputc((int)*cursor, stream);
    }
    cursor++;
  }
}

int cai_cli_review_format(const char *json, size_t length,
                          const char *output_type, char **out) {
  review_report report;
  review_finding *findings;
  lonejson *parser;
  lonejson_error error;
  lonejson_status status;
  FILE *stream;
  char *buffer;
  size_t buffer_length;
  size_t i;
  int rc;

  if (json == NULL || output_type == NULL || out == NULL)
    return -1;
  *out = NULL;
  if (strcmp(output_type, "json") == 0) {
    if (length > (size_t)-1 - 2U)
      return -1;
    buffer = (char *)malloc(length + 2U);
    if (buffer == NULL)
      return -1;
    memcpy(buffer, json, length);
    if (length == 0U || json[length - 1U] != '\n')
      buffer[length++] = '\n';
    buffer[length] = '\0';
    *out = buffer;
    return 0;
  }
  if (strcmp(output_type, "markdown") != 0)
    return -1;
  memset(&report, 0, sizeof(report));
  lonejson_error_init(&error);
  parser = lonejson_new(NULL, &error);
  if (parser == NULL)
    return -1;
  lonejson_init(parser, &report_map, &report);
  status =
      lonejson_parse_buffer(parser, &report_map, &report, json, length, &error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&report_map, &report);
    lonejson_free(parser);
    return -1;
  }
  buffer = NULL;
  buffer_length = 0U;
  stream = open_memstream(&buffer, &buffer_length);
  if (stream == NULL) {
    lonejson_cleanup(&report_map, &report);
    lonejson_free(parser);
    return -1;
  }
  fputs("# Review findings\n\n**Verdict:** ", stream);
  review_safe_text(stream, report.overall_correctness);
  fputs("\n\n", stream);
  review_safe_text(stream, report.overall_explanation);
  fputs("\n\n", stream);
  findings = (review_finding *)report.findings.items;
  if (report.findings.count == 0U)
    fputs("No actionable findings.\n", stream);
  for (i = 0U; i < report.findings.count; i++) {
    fputs("## ", stream);
    review_safe_text(stream, findings[i].title);
    fputs("\n\n", stream);
    if (findings[i].priority_present)
      fprintf(stream, "Priority: P%lld  \n", (long long)findings[i].priority);
    review_safe_text(stream, findings[i].body);
    fputs("\n\nLocation: `", stream);
    review_safe_text(stream, findings[i].code_location.absolute_file_path);
    fprintf(stream, ":%lld-%lld`\n\n",
            (long long)findings[i].code_location.line_range.start,
            (long long)findings[i].code_location.line_range.end);
  }
  rc = ferror(stream) ? -1 : 0;
  if (fclose(stream) != 0)
    rc = -1;
  lonejson_cleanup(&report_map, &report);
  lonejson_free(parser);
  if (rc != 0) {
    free(buffer);
    return -1;
  }
  *out = buffer;
  return 0;
}
