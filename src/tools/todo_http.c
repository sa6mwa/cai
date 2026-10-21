#include "../cai_internal.h"
#include "todo_internal.h"
#include <string.h>

static const unsigned char page[] = {
#include "cai_kanban_html.inc"
};
static const unsigned char script[] = {
#include "cai_kanban_js.inc"
};
static const unsigned char style[] = {
#include "cai_kanban_css.inc"
};

int cai_todo_handle_http(cai_todo *self, const cai_todo_http_request *request,
                         cai_todo_http_response *response, cai_error *error) {
  const char *resource;
  const void *bytes = "Not found\n";
  size_t count = sizeof("Not found\n") - 1U;
  int head, rc;
  if (self == NULL || request == NULL || response == NULL ||
      request->method == NULL || response->body == NULL || error == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "kanban HTTP request and sink required");
  resource = request->resource != NULL ? request->resource : "";
  head = strcmp(request->method, "HEAD") == 0;
  response->status = 404;
  response->content_type = "text/plain; charset=utf-8";
  if (!head && strcmp(request->method, "GET") != 0) {
    response->status = 405;
    bytes = "Read-only kanban route\n";
    count = sizeof("Read-only kanban route\n") - 1U;
  } else if (strcmp(resource, "board.json") == 0) {
    return cai_todo_http_snapshot(self, request->board, head, response, error);
  } else if (resource[0] == '\0' || strcmp(resource, "index.html") == 0) {
    response->status = 200;
    response->content_type = "text/html; charset=utf-8";
    bytes = page;
    count = sizeof(page) - 1U;
  } else if (strcmp(resource, "app.js") == 0) {
    response->status = 200;
    response->content_type = "text/javascript; charset=utf-8";
    bytes = script;
    count = sizeof(script) - 1U;
  } else if (strcmp(resource, "style.css") == 0) {
    response->status = 200;
    response->content_type = "text/css; charset=utf-8";
    bytes = style;
    count = sizeof(style) - 1U;
  }
  rc = response->begin != NULL
           ? response->begin(response->context, response->status,
                             response->content_type, error)
           : CAI_OK;
  return rc == CAI_OK && !head
             ? cai_sink_write(response->body, bytes, count, error)
             : rc;
}
