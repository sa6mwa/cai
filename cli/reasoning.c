#include "reasoning.h"
#include <libmdf/mdf.h>
#include <string.h>

void cai_cli_reasoning_reset(cai_cli_reasoning *reasoning) {
  memset(reasoning, 0, sizeof(*reasoning));
}
void cai_cli_reasoning_end(cai_cli_reasoning *reasoning) {
  reasoning->started = 0;
}
int cai_cli_reasoning_feed(cai_cli_reasoning *reasoning, const char *data,
                           size_t length, cai_cli_reasoning_body_fn body,
                           void *context) {
  size_t i;
  if (!reasoning->started) {
    cai_cli_reasoning_reset(reasoning);
    reasoning->started = 1;
  }
  for (i = 0U; i < length; i++) {
    char ch = data[i];
    if (reasoning->skip_lf) {
      reasoning->skip_lf = 0;
      if (ch == '\n')
        continue;
    }
    if (reasoning->phase == 0) {
      if (reasoning->header_length == 0U &&
          (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'))
        continue;
      if (ch == '\r' || ch == '\n') {
        reasoning->phase = 1;
        reasoning->skip_lf = ch == '\r';
      } else if (reasoning->header_length + 1U < sizeof(reasoning->header)) {
        reasoning->header[reasoning->header_length++] = ch == '\0' ? ' ' : ch;
        reasoning->header[reasoning->header_length] = '\0';
        reasoning->dirty = 1;
      }
    } else if (reasoning->phase == 1) {
      if (ch == '\r' || ch == '\n') {
        reasoning->indent_length = 0U;
      } else if (ch == ' ' || ch == '\t') {
        if (reasoning->indent_length < sizeof(reasoning->indent))
          reasoning->indent[reasoning->indent_length++] = ch;
      } else {
        reasoning->phase = 2;
        if (reasoning->indent_length != 0U &&
            body(context, reasoning->indent, reasoning->indent_length) != 0)
          return -1;
        return body(context, data + i, length - i);
      }
    } else {
      return body(context, data + i, length - i);
    }
  }
  return 0;
}

typedef struct title_sink {
  char *out;
  size_t length;
  size_t capacity;
} title_sink;
static int title_write(void *context, const char *data, size_t length) {
  title_sink *sink = context;
  size_t i;
  for (i = 0U; i < length && sink->length + 1U < sink->capacity; i++) {
    unsigned char ch = (unsigned char)data[i];
    if (ch < 32U || ch == 127U)
      ch = ' ';
    if (ch == ' ' &&
        (sink->length == 0U || sink->out[sink->length - 1U] == ' '))
      continue;
    sink->out[sink->length++] = (char)ch;
  }
  sink->out[sink->length] = '\0';
  return 0;
}
int cai_cli_reasoning_title(cai_cli_reasoning *reasoning, const char **out) {
  mdf_options options;
  mdf_sink sink;
  mdf *renderer;
  title_sink title;
  int rc;
  *out = reasoning->title;
  if (!reasoning->dirty)
    return 0;
  memset(&title, 0, sizeof(title));
  title.out = reasoning->title;
  title.capacity = sizeof(reasoning->title);
  title.out[0] = '\0';
  mdf_options_init(&options);
  options.width = 1024;
  options.boring = 1;
  options.ansi_mode = MDF_ANSI_OFF;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK)
    return -1;
  sink.userdata = &title;
  sink.write = title_write;
  rc = renderer->set_sink(renderer, &sink);
  if (rc == MDF_OK)
    rc = renderer->feed(renderer, reasoning->header, reasoning->header_length);
  if (rc == MDF_OK)
    rc = renderer->finish_document(renderer);
  renderer->destroy(renderer);
  if (rc != MDF_OK)
    return -1;
  if (title.length > 0U && title.out[title.length - 1U] == ' ')
    title.out[--title.length] = '\0';
  reasoning->dirty = 0;
  return 0;
}
