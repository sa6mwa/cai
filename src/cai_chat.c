#include "cai_internal.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef CAI_TESTING
static int (*cai_chat_retry_sleep_fn)(const struct timespec *,
                                      struct timespec *);
void cai_chat_test_set_retry_sleep_fn(int (*fn)(const struct timespec *,
                                                struct timespec *)) {
  cai_chat_retry_sleep_fn = fn;
}
#endif

static int cai_chat_retry_sleep(const struct timespec *delay,
                                struct timespec *remaining) {
#ifdef CAI_TESTING
  if (cai_chat_retry_sleep_fn != NULL)
    return cai_chat_retry_sleep_fn(delay, remaining);
#endif
  return nanosleep(delay, remaining);
}

typedef struct cai_chat_content_doc {
  char *type;
  lonejson_spooled text;
  lonejson_spooled refusal;
  char *image_url;
  char *detail;
} cai_chat_content_doc;

typedef struct cai_chat_history_doc {
  char *type;
  char *role;
  char *call_id;
  char *name;
  lonejson_spooled arguments;
  lonejson_spooled input;
  lonejson_spooled output;
  lonejson_object_array content;
} cai_chat_history_doc;

typedef struct cai_chat_reader {
  lonejson_spooled spool;
  const char *bytes;
  size_t length;
  size_t offset;
  int use_spool;
} cai_chat_reader;

typedef struct cai_chat_writer_context {
  size_t length;
  int fd;
  int count_only;
  int pipe_closed;
} cai_chat_writer_context;

struct cai_chat_upload {
  const cai_response_create_params *params;
  int fd[2];
  pthread_t thread;
  int thread_started;
  size_t size;
  cai_error error;
  int result;
  int pipe_closed;
};

static lonejson_status cai_chat_spool_append(void *user, const void *data,
                                             size_t len, lonejson_error *error);

static const lonejson_field cai_chat_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_content_doc, type, "type"),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_content_doc, text, "text",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_content_doc, refusal, "refusal",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_content_doc, image_url,
                                          "image_url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_content_doc, detail,
                                          "detail")};
LONEJSON_MAP_DEFINE(cai_chat_content_map, cai_chat_content_doc,
                    cai_chat_content_fields);

static const lonejson_field cai_chat_history_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_history_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_history_doc, role, "role"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_history_doc, call_id,
                                          "call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_history_doc, name, "name"),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_history_doc, arguments,
                                       "arguments",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_history_doc, input, "input",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_history_doc, output, "output",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_chat_history_doc, content, "content", cai_chat_content_doc,
        &cai_chat_content_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_chat_history_map, cai_chat_history_doc,
                    cai_chat_history_fields);

static lonejson_read_result cai_chat_read(void *user, unsigned char *buffer,
                                          size_t capacity) {
  cai_chat_reader *reader;
  lonejson_read_result result;
  size_t available;

  reader = (cai_chat_reader *)user;
  if (reader->use_spool) {
    return reader->spool.read(&reader->spool, buffer, capacity);
  }
  result = lonejson_default_read_result();
  available = reader->length - reader->offset;
  if (available > capacity) {
    available = capacity;
  }
  if (available > 0U) {
    memcpy(buffer, reader->bytes + reader->offset, available);
    reader->offset += available;
  }
  result.bytes_read = available;
  result.eof = reader->offset == reader->length;
  return result;
}

static lonejson_status cai_chat_write_sink(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  cai_chat_writer_context *context;
  size_t offset;
  ssize_t written;

  (void)error;
  context = (cai_chat_writer_context *)user;
  if (SIZE_MAX - context->length < len) {
    return LONEJSON_STATUS_OVERFLOW;
  }
  context->length += len;
  if (context->count_only) {
    return LONEJSON_STATUS_OK;
  }
  offset = 0U;
  while (offset < len) {
#ifdef MSG_NOSIGNAL
    written = send(context->fd, (const char *)data + offset, len - offset,
                   MSG_NOSIGNAL);
#else
    written = write(context->fd, (const char *)data + offset, len - offset);
#endif
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      if (written < 0 && (errno == EPIPE || errno == ECONNRESET))
        context->pipe_closed = 1;
      return LONEJSON_STATUS_IO_ERROR;
    }
    offset += (size_t)written;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_chat_key(lonejson_writer *writer, const char *key,
                                    lonejson_error *error) {
  return writer->key(writer, key, strlen(key), error);
}

static lonejson_status cai_chat_string(lonejson_writer *writer,
                                       const char *value,
                                       lonejson_error *error) {
  return writer->string(writer, value, strlen(value), error);
}

static lonejson_status cai_chat_member(lonejson_writer *writer, const char *key,
                                       const char *value,
                                       lonejson_error *error) {
  lonejson_status status;

  status = cai_chat_key(writer, key, error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_string(writer, value, error);
  }
  return status;
}

static lonejson_status cai_chat_string_spool(lonejson_writer *writer,
                                             const lonejson_spooled *spool,
                                             lonejson_error *error) {
  if (spool == NULL || spool->read == NULL) {
    return writer->string(writer, "", 0U, error);
  }
  return writer->string_spooled(writer, spool, error);
}

static lonejson_status cai_chat_text_chunks(lonejson_writer *writer,
                                            const lonejson_spooled *spool,
                                            lonejson_error *error) {
  lonejson_spooled cursor;
  lonejson_read_result read_result;
  lonejson_status status;
  unsigned char buffer[8192];

  if (spool == NULL || spool->read == NULL) {
    return LONEJSON_STATUS_OK;
  }
  cursor = *spool;
  status = cursor.rewind(&cursor, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  do {
    read_result = cursor.read(&cursor, buffer, sizeof(buffer));
    if (read_result.error_code != 0) {
      return LONEJSON_STATUS_IO_ERROR;
    }
    if (read_result.bytes_read > 0U) {
      status =
          writer->string_chunk(writer, buffer, read_result.bytes_read, error);
      if (status != LONEJSON_STATUS_OK) {
        return status;
      }
    }
  } while (!read_result.eof);
  return LONEJSON_STATUS_OK;
}

static int cai_chat_is_text(const char *type) {
  return type != NULL &&
         (strcmp(type, "input_text") == 0 || strcmp(type, "output_text") == 0 ||
          strcmp(type, "text") == 0);
}

static lonejson_status cai_chat_write_image(lonejson_writer *writer,
                                            const char *url, const char *detail,
                                            lonejson_error *error) {
  lonejson_status status;

  if (url == NULL) {
    return LONEJSON_STATUS_UNSUPPORTED;
  }
  status = writer->begin_object(writer, error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(writer, "type", "image_url", error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(writer, "image_url", error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->begin_object(writer, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(writer, "url", url, error);
  }
  if (status == LONEJSON_STATUS_OK && detail != NULL) {
    status = cai_chat_member(writer, "detail", detail, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_object(writer, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_object(writer, error);
  }
  return status;
}

static lonejson_status cai_chat_write_content(lonejson_writer *writer,
                                              const cai_chat_content_doc *parts,
                                              size_t count,
                                              lonejson_error *error) {
  lonejson_status status;
  size_t i, visible = 0U, first = 0U;

  for (i = 0U; i < count; i++) {
    if (parts[i].type != NULL && strcmp(parts[i].type, "refusal") == 0)
      continue;
    if (visible++ == 0U)
      first = i;
    if (!cai_chat_is_text(parts[i].type) &&
        strcmp(parts[i].type != NULL ? parts[i].type : "", "input_image") !=
            0) {
      return LONEJSON_STATUS_UNSUPPORTED;
    }
  }
  if (count != 0U && visible == 0U)
    return writer->null_fn(writer, error);
  if (visible == 0U || (visible == 1U && cai_chat_is_text(parts[first].type))) {
    status = writer->string_begin(writer, error);
    if (status == LONEJSON_STATUS_OK && visible == 1U) {
      status = cai_chat_text_chunks(writer, &parts[first].text, error);
    }
    return status == LONEJSON_STATUS_OK ? writer->string_end(writer, error)
                                        : status;
  }
  status = writer->begin_array(writer, error);
  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
    if (parts[i].type != NULL && strcmp(parts[i].type, "refusal") == 0)
      continue;
    if (cai_chat_is_text(parts[i].type)) {
      status = writer->begin_object(writer, error);
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_member(writer, "type", "text", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_key(writer, "text", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_string_spool(writer, &parts[i].text, error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer->end_object(writer, error);
      }
    } else {
      status = cai_chat_write_image(writer, parts[i].image_url, parts[i].detail,
                                    error);
    }
  }
  return status == LONEJSON_STATUS_OK ? writer->end_array(writer, error)
                                      : status;
}

typedef struct cai_chat_nested_string_context {
  lonejson_writer *outer;
} cai_chat_nested_string_context;

static lonejson_status cai_chat_nested_string_sink(void *user,
                                                   const void *bytes,
                                                   size_t count,
                                                   lonejson_error *error) {
  cai_chat_nested_string_context *context;

  context = (cai_chat_nested_string_context *)user;
  return context->outer->string_chunk(context->outer, bytes, count, error);
}

static lonejson_status
cai_chat_write_tool_call(lonejson_writer *writer, const char *call_id,
                         const char *name, const lonejson_spooled *arguments,
                         int is_custom, lonejson_error *error) {
  lonejson_status status;
  lonejson_writer inner;
  cai_chat_nested_string_context nested;

  status = writer->begin_object(writer, error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(writer, "id", call_id, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(writer, "type", "function", error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(writer, "function", error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->begin_object(writer, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(writer, "name", name, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(writer, "arguments", error);
  }
  if (status == LONEJSON_STATUS_OK) {
    if (is_custom) {
      status = writer->string_begin(writer, error);
      memset(&inner, 0, sizeof(inner));
      nested.outer = writer;
      if (status == LONEJSON_STATUS_OK) {
        status = CAI_LJ->writer_init_sink(
            CAI_LJ, &inner, cai_chat_nested_string_sink, &nested, error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = inner.begin_object(&inner, error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_key(&inner, "input", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_string_spool(&inner, arguments, error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = inner.end_object(&inner, error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = inner.finish(&inner, error);
      }
      if (inner.cleanup != NULL) {
        inner.cleanup(&inner);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer->string_end(writer, error);
      }
    } else {
      status = cai_chat_string_spool(writer, arguments, error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_object(writer, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_object(writer, error);
  }
  return status;
}

static lonejson_status
cai_chat_write_history_item(lonejson_writer *writer,
                            const cai_chat_history_doc *item,
                            int *tool_message_open, lonejson_error *error) {
  lonejson_status status;
  const cai_chat_content_doc *parts;
  size_t i;
  int has_refusal;
  int is_tool;

  if (item->type != NULL && strcmp(item->type, "reasoning") == 0) {
    return LONEJSON_STATUS_OK;
  }
  is_tool = item->type != NULL && (strcmp(item->type, "function_call") == 0 ||
                                   strcmp(item->type, "custom_tool_call") == 0);
  if (*tool_message_open && !is_tool) {
    status = writer->end_array(writer, error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer->end_object(writer, error);
    }
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    *tool_message_open = 0;
  }
  if (item->type == NULL || strcmp(item->type, "message") == 0) {
    if (item->role == NULL) {
      return LONEJSON_STATUS_INVALID_ARGUMENT;
    }
    status = writer->begin_object(writer, error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(writer, "role", item->role, error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(writer, "content", error);
    }
    parts = (const cai_chat_content_doc *)item->content.items;
    if (status == LONEJSON_STATUS_OK) {
      status =
          cai_chat_write_content(writer, parts, item->content.count, error);
    }
    has_refusal = 0;
    for (i = 0U; i < item->content.count; i++)
      if (parts[i].type != NULL && strcmp(parts[i].type, "refusal") == 0)
        has_refusal = 1;
    if (status == LONEJSON_STATUS_OK && has_refusal) {
      if (strcmp(item->role, "assistant") != 0)
        return LONEJSON_STATUS_UNSUPPORTED;
      status = cai_chat_key(writer, "refusal", error);
      if (status == LONEJSON_STATUS_OK)
        status = writer->string_begin(writer, error);
      for (i = 0U; status == LONEJSON_STATUS_OK && i < item->content.count; i++)
        if (parts[i].type != NULL && strcmp(parts[i].type, "refusal") == 0)
          status = cai_chat_text_chunks(writer, &parts[i].refusal, error);
      if (status == LONEJSON_STATUS_OK)
        status = writer->string_end(writer, error);
    }
  } else if (is_tool) {
    if (item->call_id == NULL || item->name == NULL) {
      return LONEJSON_STATUS_INVALID_ARGUMENT;
    }
    if (!*tool_message_open) {
      status = writer->begin_object(writer, error);
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_member(writer, "role", "assistant", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_key(writer, "tool_calls", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer->begin_array(writer, error);
      }
      if (status != LONEJSON_STATUS_OK) {
        return status;
      }
      *tool_message_open = 1;
    }
    return cai_chat_write_tool_call(
        writer, item->call_id, item->name,
        strcmp(item->type, "custom_tool_call") == 0 ? &item->input
                                                    : &item->arguments,
        strcmp(item->type, "custom_tool_call") == 0, error);
  } else if (strcmp(item->type, "function_call_output") == 0 ||
             strcmp(item->type, "custom_tool_call_output") == 0) {
    if (item->call_id == NULL) {
      return LONEJSON_STATUS_INVALID_ARGUMENT;
    }
    status = writer->begin_object(writer, error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(writer, "role", "tool", error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(writer, "tool_call_id", item->call_id, error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(writer, "content", error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_string_spool(writer, &item->output, error);
    }
  } else {
    return LONEJSON_STATUS_UNSUPPORTED;
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_object(writer, error);
  }
  return status;
}

static lonejson_status
cai_chat_write_history(lonejson_writer *writer,
                       const cai_response_create_params *params,
                       lonejson_error *error) {
  lonejson_array_stream *stream;
  lonejson_array_stream_result result;
  cai_chat_reader reader;
  cai_chat_history_doc item;
  lonejson_status status;
  int tool_message_open;

  if (!params->has_raw_input_spooled && params->raw_input_json == NULL) {
    return LONEJSON_STATUS_OK;
  }
  memset(&reader, 0, sizeof(reader));
  if (params->has_raw_input_spooled) {
    reader.spool = params->raw_input_spooled;
    reader.use_spool = 1;
    status = reader.spool.rewind(&reader.spool, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
  } else {
    reader.bytes = params->raw_input_json;
    reader.length = strlen(reader.bytes);
  }
  stream = CAI_LJ->array_stream_open_reader(CAI_LJ, "", cai_chat_read, &reader,
                                            error);
  if (stream == NULL) {
    return LONEJSON_STATUS_INVALID_JSON;
  }
  status = LONEJSON_STATUS_OK;
  tool_message_open = 0;
  for (;;) {
    memset(&item, 0, sizeof(item));
    CAI_LJ->init(CAI_LJ, &cai_chat_history_map, &item);
    result = stream->next(stream, &cai_chat_history_map, &item, error);
    if (result == LONEJSON_ARRAY_STREAM_EOF) {
      CAI_LJ->cleanup(CAI_LJ, &cai_chat_history_map, &item);
      break;
    }
    if (result != LONEJSON_ARRAY_STREAM_ITEM) {
      status = LONEJSON_STATUS_INVALID_JSON;
      CAI_LJ->cleanup(CAI_LJ, &cai_chat_history_map, &item);
      break;
    }
    status =
        cai_chat_write_history_item(writer, &item, &tool_message_open, error);
    CAI_LJ->cleanup(CAI_LJ, &cai_chat_history_map, &item);
    if (status != LONEJSON_STATUS_OK) {
      break;
    }
  }
  stream->close(stream);
  if (status == LONEJSON_STATUS_OK && tool_message_open) {
    status = writer->end_array(writer, error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer->end_object(writer, error);
    }
  }
  return status;
}

static lonejson_status
cai_chat_write_input_parts(lonejson_writer *writer,
                           const struct cai_content_part *parts, size_t count,
                           int tool_output, lonejson_error *error) {
  lonejson_status status;
  size_t i;

  if (tool_output || count == 0U ||
      (count == 1U && cai_chat_is_text(parts[0].type))) {
    status = writer->string_begin(writer, error);
    for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
      if (i != 0U)
        status = writer->string_chunk(writer, "\n", 1U, error);
      if (status != LONEJSON_STATUS_OK)
        break;
      if (cai_chat_is_text(parts[i].type)) {
        if (parts[i].has_text_spooled)
          status = cai_chat_text_chunks(writer, &parts[i].text_spooled, error);
        else if (parts[i].text != NULL)
          status = writer->string_chunk(writer, parts[i].text,
                                        strlen(parts[i].text), error);
      } else if (parts[i].type != NULL &&
                 strcmp(parts[i].type, "input_image") == 0 &&
                 parts[i].image_url != NULL) {
        static const char acknowledgement[] =
            "Image returned by tool; attached in the following user message.";
        status = writer->string_chunk(writer, acknowledgement,
                                      sizeof(acknowledgement) - 1U, error);
      } else {
        return LONEJSON_STATUS_UNSUPPORTED;
      }
    }
    return status == LONEJSON_STATUS_OK ? writer->string_end(writer, error)
                                        : status;
  }
  status = writer->begin_array(writer, error);
  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
    if (cai_chat_is_text(parts[i].type)) {
      status = writer->begin_object(writer, error);
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_member(writer, "type", "text", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_key(writer, "text", error);
      }
      if (status == LONEJSON_STATUS_OK && parts[i].has_text_spooled) {
        status = cai_chat_string_spool(writer, &parts[i].text_spooled, error);
      } else if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_string(
            writer, parts[i].text != NULL ? parts[i].text : "", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer->end_object(writer, error);
      }
    } else if (parts[i].type != NULL &&
               strcmp(parts[i].type, "input_image") == 0) {
      status = cai_chat_write_image(writer, parts[i].image_url, parts[i].detail,
                                    error);
    } else {
      return LONEJSON_STATUS_UNSUPPORTED;
    }
  }
  return status == LONEJSON_STATUS_OK ? writer->end_array(writer, error)
                                      : status;
}

/* Tool messages only support text. Attach images after the entire tool-result
 * group, so no user message separates a call from its pending results. */
static lonejson_status
cai_chat_write_tool_images(lonejson_writer *writer,
                           const struct cai_input_message *items, size_t count,
                           lonejson_error *error) {
  const struct cai_content_part *parts;
  lonejson_status status = LONEJSON_STATUS_OK;
  size_t i, j;
  int open = 0;

  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
    parts = (const struct cai_content_part *)items[i].content.items;
    for (j = 0U; status == LONEJSON_STATUS_OK && j < items[i].content.count;
         j++) {
      if (parts[j].type == NULL || strcmp(parts[j].type, "input_image") != 0)
        continue;
      if (!open) {
        status = writer->begin_object(writer, error);
        if (status == LONEJSON_STATUS_OK)
          status = cai_chat_member(writer, "role", "user", error);
        if (status == LONEJSON_STATUS_OK)
          status = cai_chat_key(writer, "content", error);
        if (status == LONEJSON_STATUS_OK)
          status = writer->begin_array(writer, error);
        open = 1;
      }
      if (status == LONEJSON_STATUS_OK)
        status = writer->begin_object(writer, error);
      if (status == LONEJSON_STATUS_OK)
        status = cai_chat_member(writer, "type", "text", error);
      if (status == LONEJSON_STATUS_OK)
        status = cai_chat_key(writer, "text", error);
      if (status == LONEJSON_STATUS_OK)
        status = writer->string_begin(writer, error);
      if (status == LONEJSON_STATUS_OK) {
        static const char prefix[] = "Image from tool call ";
        status =
            writer->string_chunk(writer, prefix, sizeof(prefix) - 1U, error);
      }
      if (status == LONEJSON_STATUS_OK)
        status = writer->string_chunk(writer, items[i].call_id,
                                      strlen(items[i].call_id), error);
      if (status == LONEJSON_STATUS_OK)
        status = writer->string_end(writer, error);
      if (status == LONEJSON_STATUS_OK)
        status = writer->end_object(writer, error);
      if (status == LONEJSON_STATUS_OK)
        status = cai_chat_write_image(writer, parts[j].image_url,
                                      parts[j].detail, error);
    }
  }
  if (status == LONEJSON_STATUS_OK && open)
    status = writer->end_array(writer, error);
  if (status == LONEJSON_STATUS_OK && open)
    status = writer->end_object(writer, error);
  return status;
}

static lonejson_status
cai_chat_write_input(lonejson_writer *writer,
                     const cai_response_create_params *params,
                     lonejson_error *error) {
  const struct cai_input_message *items;
  const struct cai_content_part *parts;
  lonejson_status status;
  size_t i, tool_start = 0U;

  items = (const struct cai_input_message *)params->input.items;
  for (i = 0U; i < params->input.count; i++) {
    status = writer->begin_object(writer, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    if (items[i].kind == CAI_INPUT_MESSAGE) {
      if (items[i].role == NULL) {
        return LONEJSON_STATUS_INVALID_ARGUMENT;
      }
      status = cai_chat_member(writer, "role", items[i].role, error);
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_key(writer, "content", error);
      }
      if (status == LONEJSON_STATUS_OK) {
        parts = (const struct cai_content_part *)items[i].content.items;
        status = cai_chat_write_input_parts(writer, parts,
                                            items[i].content.count, 0, error);
      }
    } else if (items[i].kind == CAI_INPUT_FUNCTION_CALL_OUTPUT ||
               items[i].kind == CAI_INPUT_CUSTOM_TOOL_CALL_OUTPUT) {
      if (items[i].call_id == NULL) {
        return LONEJSON_STATUS_INVALID_ARGUMENT;
      }
      status = cai_chat_member(writer, "role", "tool", error);
      if (status == LONEJSON_STATUS_OK) {
        status =
            cai_chat_member(writer, "tool_call_id", items[i].call_id, error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_key(writer, "content", error);
      }
      if (status == LONEJSON_STATUS_OK && items[i].content.count > 0U) {
        parts = (const struct cai_content_part *)items[i].content.items;
        status = cai_chat_write_input_parts(writer, parts,
                                            items[i].content.count, 1, error);
      } else if (status == LONEJSON_STATUS_OK && items[i].has_output_spooled) {
        status = cai_chat_string_spool(writer, &items[i].output_spooled, error);
      } else if (status == LONEJSON_STATUS_OK) {
        status = cai_chat_string(
            writer, items[i].output != NULL ? items[i].output : "", error);
      }
    } else {
      return LONEJSON_STATUS_UNSUPPORTED;
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer->end_object(writer, error);
    }
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    if (items[i].kind == CAI_INPUT_MESSAGE) {
      tool_start = i + 1U;
    } else if (i + 1U == params->input.count ||
               items[i + 1U].kind == CAI_INPUT_MESSAGE) {
      status = cai_chat_write_tool_images(writer, items + tool_start,
                                          i + 1U - tool_start, error);
      if (status != LONEJSON_STATUS_OK)
        return status;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
cai_chat_write_tools(lonejson_writer *writer,
                     const cai_response_create_params *params,
                     lonejson_error *error) {
  const struct cai_function_tool *tools;
  lonejson_status status;
  size_t i;

  tools = (const struct cai_function_tool *)params->tools.items;
  status = cai_chat_key(writer, "tools", error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer->begin_array(writer, error);
  }
  for (i = 0U; status == LONEJSON_STATUS_OK && i < params->tools.count; i++) {
    if (tools[i].is_raw || tools[i].name == NULL ||
        (!tools[i].is_custom && tools[i].parameters_json == NULL)) {
      return LONEJSON_STATUS_UNSUPPORTED;
    }
    status = writer->begin_object(writer, error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(writer, "type", "function", error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(writer, "function", error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer->begin_object(writer, error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(writer, "name", tools[i].name, error);
    }
    if (status == LONEJSON_STATUS_OK && tools[i].description != NULL) {
      status =
          cai_chat_member(writer, "description", tools[i].description, error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(writer, "parameters", error);
    }
    if (status == LONEJSON_STATUS_OK) {
      if (tools[i].is_custom) {
        static const char custom_parameters[] =
            "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":"
            "\"string\"}},"
            "\"required\":[\"input\"]}";
        status = writer->json_value_buffer(
            writer, custom_parameters, sizeof(custom_parameters) - 1U, error);
      } else {
        status =
            writer->json_value_buffer(writer, tools[i].parameters_json,
                                      strlen(tools[i].parameters_json), error);
      }
    }
    if (status == LONEJSON_STATUS_OK && !tools[i].is_custom) {
      status = cai_chat_key(writer, "strict", error);
      if (status == LONEJSON_STATUS_OK) {
        status = writer->bool_fn(writer, tools[i].strict != 0, error);
      }
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer->end_object(writer, error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer->end_object(writer, error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_array(writer, error);
  }
  return status;
}

static int cai_chat_request_write(const cai_response_create_params *params,
                                  cai_chat_writer_context *context,
                                  cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  if (params->previous_response_id != NULL || params->conversation_id != NULL ||
      params->background || params->compact_threshold_tokens > 0LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chat Completions cannot use Responses server state");
  }
  if (params->tool_choice_json != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "raw Responses tool choice is unsupported by Chat "
                         "Completions");
  }
  memset(&writer, 0, sizeof(writer));
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_chat_write_sink,
                                    context, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "model", params->model, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "messages", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && params->instructions != NULL) {
    status = writer.begin_object(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(&writer, "role", "system", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(&writer, "content", params->instructions,
                               &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.end_object(&writer, &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_write_history(&writer, params, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_write_input(&writer, params, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "stream", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.bool_fn(&writer, 1, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "stream_options", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "include_usage", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.bool_fn(&writer, 1, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && params->reasoning_effort != NULL) {
    status = cai_chat_member(&writer, "reasoning_effort",
                             params->reasoning_effort, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && params->service_tier != NULL)
    status = cai_chat_member(&writer, "service_tier", params->service_tier,
                             &json_error);
  if (status == LONEJSON_STATUS_OK && params->max_output_tokens > 0) {
    status = cai_chat_key(&writer, "max_completion_tokens", &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.i64(&writer, params->max_output_tokens, &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK && params->tools.count > 0U) {
    status = cai_chat_write_tools(&writer, params, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && params->tool_choice != NULL &&
      params->tools.count > 0U) {
    status = cai_chat_member(&writer, "tool_choice", params->tool_choice,
                             &json_error);
  }
  if (status == LONEJSON_STATUS_OK && params->parallel_tool_calls >= 0 &&
      params->tools.count > 0U) {
    status = cai_chat_key(&writer, "parallel_tool_calls", &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.bool_fn(&writer, params->parallel_tool_calls != 0,
                              &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK && params->text_format_schema_json != NULL) {
    status = cai_chat_key(&writer, "response_format", &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.begin_object(&writer, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(&writer, "type", "json_schema", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(&writer, "json_schema", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.begin_object(&writer, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(&writer, "name",
                               params->text_format_name != NULL
                                   ? params->text_format_name
                                   : "response",
                               &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(&writer, "strict", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status =
          writer.bool_fn(&writer, params->text_format_strict != 0, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_key(&writer, "schema", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.json_value_buffer(
          &writer, params->text_format_schema_json,
          strlen(params->text_format_schema_json), &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.end_object(&writer, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.end_object(&writer, &json_error);
    }
  } else if (status == LONEJSON_STATUS_OK && params->text_format_type != NULL &&
             strcmp(params->text_format_type, "json_object") == 0) {
    status = cai_chat_key(&writer, "response_format", &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.begin_object(&writer, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_chat_member(&writer, "type", "json_object", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.end_object(&writer, &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer.cleanup != NULL) {
    writer.cleanup(&writer);
  }
  if (status == LONEJSON_STATUS_UNSUPPORTED) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Berget Chat Completions supports image URLs, text "
                         "and function tools; file IDs, file attachments and "
                         "provider-hosted tools require another API");
  }
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to create Chat Completions request",
                                json_error.message);
  }
  return CAI_OK;
}

static void *cai_chat_upload_worker(void *arg) {
  cai_chat_upload *upload;
  cai_chat_writer_context context;

  upload = (cai_chat_upload *)arg;
  memset(&context, 0, sizeof(context));
  context.fd = upload->fd[1];
  cai_error_init(&upload->error);
  upload->result =
      cai_chat_request_write(upload->params, &context, &upload->error);
  upload->pipe_closed = context.pipe_closed;
  shutdown(upload->fd[1], SHUT_WR);
  return NULL;
}

int cai_chat_upload_open(const cai_response_create_params *params,
                         cai_chat_upload **out, cai_error *error) {
  cai_chat_writer_context context;
  cai_chat_upload *upload;
  int rc;

  if (params == NULL || params->model == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chat Completions model and request are required");
  }
  *out = NULL;
  memset(&context, 0, sizeof(context));
  context.count_only = 1;
  rc = cai_chat_request_write(params, &context, error);
  if (rc != CAI_OK) {
    return rc;
  }
  upload = (cai_chat_upload *)cai_alloc(NULL, sizeof(*upload));
  if (upload == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate Chat Completions upload");
  }
  memset(upload, 0, sizeof(*upload));
  upload->fd[0] = -1;
  upload->fd[1] = -1;
  upload->size = context.length;
  upload->params = params;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, upload->fd) != 0) {
    cai_free_mem(NULL, upload);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open Chat Completions upload pipe");
  }
  if (pthread_create(&upload->thread, NULL, cai_chat_upload_worker, upload) !=
      0) {
    close(upload->fd[0]);
    close(upload->fd[1]);
    cai_free_mem(NULL, upload);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start Chat Completions upload");
  }
  upload->thread_started = 1;
  *out = upload;
  return CAI_OK;
}

size_t cai_chat_upload_read(char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
  cai_chat_upload *upload;
  ssize_t read_result;

  upload = (cai_chat_upload *)userdata;
  if (upload == NULL || size == 0U || nmemb == 0U) {
    return CURL_READFUNC_ABORT;
  }
  do {
    read_result = recv(upload->fd[0], ptr, size * nmemb, 0);
  } while (read_result < 0 && errno == EINTR);
  return read_result >= 0 ? (size_t)read_result : CURL_READFUNC_ABORT;
}

curl_off_t cai_chat_upload_size(const cai_chat_upload *upload) {
  return upload != NULL ? (curl_off_t)upload->size : (curl_off_t)-1;
}

int cai_chat_upload_close(cai_chat_upload *upload, int *out_pipe_closed,
                          cai_error *error) {
  int rc;

  if (out_pipe_closed != NULL)
    *out_pipe_closed = 0;
  if (upload == NULL) {
    return CAI_OK;
  }
  shutdown(upload->fd[0], SHUT_RDWR);
  close(upload->fd[0]);
  if (upload->thread_started) {
    pthread_join(upload->thread, NULL);
  }
  if (out_pipe_closed != NULL)
    *out_pipe_closed = upload->pipe_closed;
  close(upload->fd[1]);
  rc = upload->result;
  if (rc != CAI_OK && error != NULL && error->code == CAI_OK) {
    cai_set_error_detail(error, upload->error.code,
                         upload->error.message != NULL
                             ? upload->error.message
                             : "Chat Completions upload failed",
                         upload->error.detail);
  }
  cai_error_cleanup(&upload->error);
  cai_free_mem(NULL, upload);
  return rc;
}

typedef struct cai_chat_chunk_function_doc {
  char *name;
  lonejson_spooled arguments;
} cai_chat_chunk_function_doc;

typedef struct cai_chat_chunk_tool_doc {
  long long index;
  char *id;
  cai_chat_chunk_function_doc function;
} cai_chat_chunk_tool_doc;

typedef struct cai_chat_chunk_delta_doc {
  lonejson_spooled content;
  lonejson_spooled refusal;
  lonejson_spooled reasoning_content;
  lonejson_object_array tool_calls;
} cai_chat_chunk_delta_doc;

typedef struct cai_chat_chunk_choice_doc {
  long long index;
  char *finish_reason;
  cai_chat_chunk_delta_doc delta;
} cai_chat_chunk_choice_doc;

typedef struct cai_chat_usage_detail_doc {
  long long cached_tokens;
  long long reasoning_tokens;
} cai_chat_usage_detail_doc;

typedef struct cai_chat_usage_doc {
  long long prompt_tokens;
  long long completion_tokens;
  long long total_tokens;
  cai_chat_usage_detail_doc prompt_tokens_details;
  cai_chat_usage_detail_doc completion_tokens_details;
} cai_chat_usage_doc;

typedef struct cai_chat_error_doc {
  char *message;
  char *code;
  char *type;
} cai_chat_error_doc;

typedef struct cai_chat_chunk_doc {
  char *id;
  lonejson_object_array choices;
  cai_chat_usage_doc usage;
  cai_chat_error_doc error;
} cai_chat_chunk_doc;

typedef struct cai_chat_custom_stream cai_chat_custom_stream;
typedef struct cai_chat_tool_state {
  char *id;
  char *name;
  lonejson_spooled arguments;
  int has_arguments;
  int is_custom;
  int delta_started;
  cai_chat_custom_stream *custom;
} cai_chat_tool_state;

typedef struct cai_chat_stream_state {
  cai_client *client;
  const cai_response_create_params *params;
  const cai_stream_sinks *sinks;
  cai_error *error;
  char **out_response_id;
  cai_token_usage *out_usage;
  cai_token_usage usage;
  char **request_id;
  long http_status;
  lonejson_sse *sse;
  lonejson_spooled event;
  lonejson_spooled text;
  lonejson_spooled refusal;
  cai_chat_tool_state *tools;
  size_t tool_count;
  char *body;
  size_t body_length;
  size_t response_bytes;
  size_t capture_limit_bytes;
  size_t capture_bytes;
  double started_at;
  double first_token_ms;
  long retry_after_ms;
  int model_activity;
  int failure_code;
  int completed;
  int length_finish;
  int done_seen;
  int text_started;
  int refusal_started;
  int visible_started;
  int reasoning_started;
  int saw_usage;
  int glm_thinking;
  int glm_thinking_started;
  int glm_thinking_done;
  size_t glm_start_match;
  size_t glm_end_match;
} cai_chat_stream_state;

static double cai_chat_now(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0.0;
  return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

typedef struct cai_chat_custom_arguments_doc {
  lonejson_spooled input;
} cai_chat_custom_arguments_doc;

static const lonejson_field cai_chat_custom_arguments_fields[] = {
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_chat_custom_arguments_doc, input,
                                     "input")};
LONEJSON_MAP_DEFINE(cai_chat_custom_arguments_map,
                    cai_chat_custom_arguments_doc,
                    cai_chat_custom_arguments_fields);

static const lonejson_field cai_chat_chunk_function_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_chunk_function_doc, name,
                                          "name"),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_chunk_function_doc, arguments,
                                       "arguments",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT)};
LONEJSON_MAP_DEFINE(cai_chat_chunk_function_map, cai_chat_chunk_function_doc,
                    cai_chat_chunk_function_fields);

static const lonejson_field cai_chat_chunk_tool_fields[] = {
    LONEJSON_FIELD_I64(cai_chat_chunk_tool_doc, index, "index"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_chunk_tool_doc, id, "id"),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_chat_chunk_tool_doc, function,
                                     "function", &cai_chat_chunk_function_map)};
LONEJSON_MAP_DEFINE(cai_chat_chunk_tool_map, cai_chat_chunk_tool_doc,
                    cai_chat_chunk_tool_fields);

static const lonejson_field cai_chat_chunk_delta_fields[] = {
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_chunk_delta_doc, content,
                                       "content",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_chunk_delta_doc, refusal,
                                       "refusal",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_chat_chunk_delta_doc,
                                       reasoning_content, "reasoning_content",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_chat_chunk_delta_doc, tool_calls, "tool_calls",
        cai_chat_chunk_tool_doc, &cai_chat_chunk_tool_map,
        LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_chat_chunk_delta_map, cai_chat_chunk_delta_doc,
                    cai_chat_chunk_delta_fields);

static const lonejson_field cai_chat_chunk_choice_fields[] = {
    LONEJSON_FIELD_I64(cai_chat_chunk_choice_doc, index, "index"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_chunk_choice_doc,
                                          finish_reason, "finish_reason"),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_chat_chunk_choice_doc, delta, "delta",
                                     &cai_chat_chunk_delta_map)};
LONEJSON_MAP_DEFINE(cai_chat_chunk_choice_map, cai_chat_chunk_choice_doc,
                    cai_chat_chunk_choice_fields);

static const lonejson_field cai_chat_usage_prompt_fields[] = {
    LONEJSON_FIELD_I64(cai_chat_usage_detail_doc, cached_tokens,
                       "cached_tokens")};
LONEJSON_MAP_DEFINE(cai_chat_usage_prompt_map, cai_chat_usage_detail_doc,
                    cai_chat_usage_prompt_fields);

static const lonejson_field cai_chat_usage_completion_fields[] = {
    LONEJSON_FIELD_I64(cai_chat_usage_detail_doc, reasoning_tokens,
                       "reasoning_tokens")};
LONEJSON_MAP_DEFINE(cai_chat_usage_completion_map, cai_chat_usage_detail_doc,
                    cai_chat_usage_completion_fields);

static const lonejson_field cai_chat_usage_fields[] = {
    LONEJSON_FIELD_I64(cai_chat_usage_doc, prompt_tokens, "prompt_tokens"),
    LONEJSON_FIELD_I64(cai_chat_usage_doc, completion_tokens,
                       "completion_tokens"),
    LONEJSON_FIELD_I64(cai_chat_usage_doc, total_tokens, "total_tokens"),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_chat_usage_doc, prompt_tokens_details,
                                     "prompt_tokens_details",
                                     &cai_chat_usage_prompt_map),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(
        cai_chat_usage_doc, completion_tokens_details,
        "completion_tokens_details", &cai_chat_usage_completion_map)};
LONEJSON_MAP_DEFINE(cai_chat_usage_map, cai_chat_usage_doc,
                    cai_chat_usage_fields);

static const lonejson_field cai_chat_error_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_error_doc, message,
                                          "message"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_error_doc, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_error_doc, type, "type")};
LONEJSON_MAP_DEFINE(cai_chat_error_map, cai_chat_error_doc,
                    cai_chat_error_fields);

static const lonejson_field cai_chat_chunk_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chat_chunk_doc, id, "id"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_chat_chunk_doc, choices, "choices", cai_chat_chunk_choice_doc,
        &cai_chat_chunk_choice_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_chat_chunk_doc, usage, "usage",
                                     &cai_chat_usage_map),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_chat_chunk_doc, error, "error",
                                     &cai_chat_error_map)};
LONEJSON_MAP_DEFINE(cai_chat_chunk_map, cai_chat_chunk_doc,
                    cai_chat_chunk_fields);

static int cai_chat_emit_affix(cai_sink *sink, const cai_stream_affix *affix,
                               cai_error *error) {
  const char *value;

  if (sink == NULL || affix == NULL) {
    return CAI_OK;
  }
  value = affix->text;
  if (value == NULL && affix->callback != NULL) {
    value = affix->callback(affix->context);
  }
  return value != NULL ? cai_sink_write(sink, value, strlen(value), error)
                       : CAI_OK;
}

typedef struct cai_chat_sink_context {
  cai_sink *sink;
  cai_error *error;
} cai_chat_sink_context;

static lonejson_status cai_chat_spool_sink(void *user, const void *bytes,
                                           size_t count,
                                           lonejson_error *json_error) {
  cai_chat_sink_context *context;

  (void)json_error;
  context = (cai_chat_sink_context *)user;
  return cai_sink_write(context->sink, bytes, count, context->error) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_CALLBACK_FAILED;
}

static int cai_chat_emit_spool(cai_sink *sink, const lonejson_spooled *spool,
                               cai_error *error) {
  lonejson_error json_error;
  cai_chat_sink_context context;

  if (sink == NULL || spool == NULL || spool->size_fn(spool) == 0U) {
    return CAI_OK;
  }
  context.sink = sink;
  context.error = error;
  lonejson_error_init(&json_error);
  if (spool->write_to_sink(spool, cai_chat_spool_sink, &context, &json_error) !=
      LONEJSON_STATUS_OK) {
    return error != NULL && error->code != CAI_OK
               ? error->code
               : cai_set_error(error, CAI_ERR_TRANSPORT,
                               "Chat Completions stream sink failed");
  }
  return CAI_OK;
}

/* Only buffered calls set this limit. Reject payload growth before retention;
 * the final JSON sink also counts escaping and response-envelope bytes. */
static int cai_chat_capture_reserve(cai_chat_stream_state *state,
                                    size_t count) {
  if (state->capture_limit_bytes == 0U)
    return CAI_OK;
  if (count > state->capture_limit_bytes - state->capture_bytes)
    return cai_set_error(
        state->error, CAI_ERR_LIMIT,
        "Chat Completions response exceeded configured JSON response limit");
  state->capture_bytes += count;
  return CAI_OK;
}

static lonejson_status cai_chat_spool_append(void *user, const void *data,
                                             size_t len, lonejson_error *error);

static int cai_chat_finish_reasoning(cai_chat_stream_state *state) {
  int rc = CAI_OK;
  if (state->reasoning_started) {
    rc = cai_chat_emit_affix(state->sinks->reasoning_summary,
                             &state->sinks->reasoning_summary_suffix,
                             state->error);
    if (rc == CAI_OK)
      state->reasoning_started = 0;
  }
  return rc;
}

typedef enum cai_chat_text_kind {
  CAI_CHAT_TEXT,
  CAI_CHAT_REASONING,
  CAI_CHAT_REFUSAL
} cai_chat_text_kind;
static int cai_chat_emit_text(cai_chat_stream_state *state,
                              const lonejson_spooled *text,
                              cai_chat_text_kind kind) {
  cai_sink *sink;
  const cai_stream_affix *prefix;
  int *started;
  int rc;

  if (text->size_fn(text) == 0U) {
    return CAI_OK;
  }
  if (kind != CAI_CHAT_REASONING) {
    rc = cai_chat_finish_reasoning(state);
    if (rc != CAI_OK)
      return rc;
    rc = cai_chat_capture_reserve(state, text->size_fn(text));
    if (rc != CAI_OK)
      return rc;
    if (state->sinks->output_item_done != NULL) {
      lonejson_error json_error;
      lonejson_error_init(&json_error);
      if (text->write_to_sink(text, cai_chat_spool_append,
                              kind == CAI_CHAT_REFUSAL ? &state->refusal
                                                       : &state->text,
                              &json_error) != LONEJSON_STATUS_OK)
        return cai_set_error_detail(state->error, CAI_ERR_TRANSPORT,
                                    "failed to retain Chat message text",
                                    json_error.message);
    }
    if (kind == CAI_CHAT_REFUSAL)
      state->refusal_started = 1;
    else
      state->text_started = 1;
  }
  sink = kind == CAI_CHAT_REASONING ? state->sinks->reasoning_summary
                                    : state->sinks->output_text;
  prefix = kind == CAI_CHAT_REASONING ? &state->sinks->reasoning_summary_prefix
                                      : &state->sinks->output_text_prefix;
  started = kind == CAI_CHAT_REASONING ? &state->reasoning_started
                                       : &state->visible_started;
  if (!*started) {
    rc = cai_chat_emit_affix(sink, prefix, state->error);
    if (rc != CAI_OK) {
      return rc;
    }
    *started = 1;
  }
  if (kind != CAI_CHAT_REASONING && state->sinks->output_text_delta != NULL) {
    rc = state->sinks->output_text_delta(state->sinks->output_text_context,
                                         "chat_text", 0, text, state->error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return cai_chat_emit_spool(sink, text, state->error);
}

/* Berget may send reasoning_content, a tagged <think> section in content,
 * or plain final content. Detect the opening tag with a bounded prefix so
 * plain answers can stream immediately without retaining the whole reply. */
static int cai_chat_emit_glm_content(cai_chat_stream_state *state,
                                     const lonejson_spooled *text) {
  static const char start_tag[] = "<think>";
  static const char end_tag[] = "</think>";
  lonejson_spooled cursor;
  lonejson_spooled visible;
  lonejson_read_result read_result;
  lonejson_error json_error;
  unsigned char buffer[8192];
  size_t i;
  int rc;

  if (text->size_fn(text) == 0U) {
    return CAI_OK;
  }
  if (state->glm_thinking_done) {
    return cai_chat_emit_text(state, text, CAI_CHAT_TEXT);
  }
  cursor = *text;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "failed to read GLM content");
  }
  CAI_LJ->spooled_init(CAI_LJ, &visible);
  do {
    read_result = cursor.read(&cursor, buffer, sizeof(buffer));
    if (read_result.error_code != 0) {
      visible.cleanup(&visible);
      return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                           "failed to read GLM content");
    }
    for (i = 0U; i < read_result.bytes_read; i++) {
      if (!state->glm_thinking_started && !state->glm_thinking_done) {
        if (buffer[i] == (unsigned char)start_tag[state->glm_start_match]) {
          state->glm_start_match++;
          if (state->glm_start_match == sizeof(start_tag) - 1U) {
            state->glm_thinking_started = 1;
            state->glm_start_match = 0U;
          }
          continue;
        }
        if (state->glm_start_match > 0U &&
            visible.append(&visible, start_tag, state->glm_start_match,
                           &json_error) != LONEJSON_STATUS_OK) {
          visible.cleanup(&visible);
          return cai_set_error(state->error, CAI_ERR_TRANSPORT,
                               "failed to retain GLM final output");
        }
        state->glm_start_match = 0U;
        state->glm_thinking_done = 1;
      }
      if (!state->glm_thinking_done) {
        if (buffer[i] == (unsigned char)end_tag[state->glm_end_match]) {
          state->glm_end_match++;
          if (state->glm_end_match == sizeof(end_tag) - 1U) {
            state->glm_thinking_done = 1;
          }
        } else {
          state->glm_end_match =
              buffer[i] == (unsigned char)end_tag[0] ? 1U : 0U;
        }
      } else if (visible.append(&visible, buffer + i, 1U, &json_error) !=
                 LONEJSON_STATUS_OK) {
        visible.cleanup(&visible);
        return cai_set_error(state->error, CAI_ERR_TRANSPORT,
                             "failed to retain GLM final output");
      }
    }
  } while (!read_result.eof);
  rc = cai_chat_emit_text(state, &visible, CAI_CHAT_TEXT);
  visible.cleanup(&visible);
  return rc;
}

static int cai_chat_flush_glm_prefix(cai_chat_stream_state *state) {
  static const char start_tag[] = "<think>";
  lonejson_spooled visible;
  lonejson_error json_error;
  int rc;
  if (!state->completed || !state->glm_thinking ||
      state->glm_thinking_started || state->glm_thinking_done ||
      state->glm_start_match == 0U)
    return CAI_OK;
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, &visible);
  if (visible.append(&visible, start_tag, state->glm_start_match,
                     &json_error) != LONEJSON_STATUS_OK) {
    visible.cleanup(&visible);
    return cai_set_error(state->error, CAI_ERR_TRANSPORT,
                         "failed to retain GLM final output");
  }
  rc = cai_chat_emit_text(state, &visible, CAI_CHAT_TEXT);
  visible.cleanup(&visible);
  if (rc == CAI_OK)
    state->glm_thinking_done = 1;
  return rc;
}

static int cai_chat_tool_append(cai_chat_tool_state *tool,
                                const lonejson_spooled *fragment,
                                cai_error *error) {
  lonejson_spooled cursor;
  lonejson_read_result result;
  lonejson_error json_error;
  unsigned char buffer[8192];

  cursor = *fragment;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to read Chat Completions tool arguments");
  }
  do {
    result = cursor.read(&cursor, buffer, sizeof(buffer));
    if (result.error_code != 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to read Chat Completions tool arguments");
    }
    if (result.bytes_read > 0U &&
        tool->arguments.append(&tool->arguments, buffer, result.bytes_read,
                               &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to retain tool arguments",
                                  json_error.message);
    }
  } while (!result.eof);
  return CAI_OK;
}

#include "cai_chat_custom.inc"

static int cai_chat_emit_tool_delta(cai_chat_stream_state *state,
                                    const cai_chat_chunk_tool_doc *delta) {
  cai_chat_tool_state *tool;
  cai_chat_tool_state *grown;
  size_t index;
  int rc;

  if (delta->index < 0LL || delta->index > 1023LL) {
    return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "invalid Chat Completions tool index");
  }
  index = (size_t)delta->index;
  if (index >= state->tool_count) {
    grown = (cai_chat_tool_state *)cai_realloc_mem(
        NULL, state->tools, (index + 1U) * sizeof(*grown));
    if (grown == NULL) {
      return cai_set_error(state->error, CAI_ERR_NOMEM,
                           "failed to allocate Chat Completions tool state");
    }
    memset(grown + state->tool_count, 0,
           (index + 1U - state->tool_count) * sizeof(*grown));
    state->tools = grown;
    state->tool_count = index + 1U;
  }
  tool = &state->tools[index];
  if (delta->id != NULL) {
    if (tool->id != NULL && strcmp(tool->id, delta->id) != 0) {
      return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                           "Chat Completions tool id changed mid-stream");
    }
    if (tool->id == NULL) {
      tool->id = cai_strdup(NULL, delta->id);
      if (tool->id == NULL) {
        return CAI_ERR_NOMEM;
      }
    }
  }
  if (delta->function.name != NULL) {
    if (tool->name != NULL && strcmp(tool->name, delta->function.name) != 0) {
      return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                           "Chat Completions tool name changed mid-stream");
    }
    if (tool->name == NULL) {
      tool->name = cai_strdup(NULL, delta->function.name);
      if (tool->name == NULL) {
        return CAI_ERR_NOMEM;
      }
      if (state->params != NULL) {
        const struct cai_function_tool *registered;
        size_t i;

        registered =
            (const struct cai_function_tool *)state->params->tools.items;
        for (i = 0U; i < state->params->tools.count; i++) {
          if (registered[i].name != NULL &&
              strcmp(registered[i].name, tool->name) == 0) {
            tool->is_custom = registered[i].is_custom;
            break;
          }
        }
      }
    }
  }
  if (delta->function.arguments.size_fn(&delta->function.arguments) > 0U) {
    rc = cai_chat_capture_reserve(
        state, delta->function.arguments.size_fn(&delta->function.arguments));
    if (rc != CAI_OK)
      return rc;
    if (!tool->has_arguments) {
      CAI_LJ->spooled_init(CAI_LJ, &tool->arguments);
      tool->has_arguments = 1;
    }
    rc = cai_chat_tool_append(tool, &delta->function.arguments, state->error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  /* Metadata may arrive after arguments. Retain that prefix until its tool
   * kind and real ID are known, then feed it once; later fragments flow live.
   */
  if (tool->id != NULL && tool->name != NULL && tool->has_arguments) {
    const lonejson_spooled *fragment =
        tool->delta_started ? &delta->function.arguments : &tool->arguments;
    tool->delta_started = 1;
    if (tool->is_custom && state->sinks->custom_tool_call_input_delta != NULL)
      return cai_chat_custom_delta(state, tool, (int)index + 1, fragment);
    if (!tool->is_custom && fragment->size_fn(fragment) > 0U &&
        state->sinks->function_call_arguments_delta != NULL)
      return state->sinks->function_call_arguments_delta(
          state->sinks->function_call_context, tool->id, (int)index + 1,
          fragment, state->error);
  }
  return CAI_OK;
}

static lonejson_status cai_chat_spool_append(void *user, const void *data,
                                             size_t len,
                                             lonejson_error *error) {
  lonejson_spooled *spool;

  spool = (lonejson_spooled *)user;
  return spool->append(spool, data, len, error);
}

static int cai_chat_emit_message_done(cai_chat_stream_state *state) {
  lonejson_spooled item;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int part;
  int rc;

  if ((!state->text_started && !state->refusal_started) ||
      state->sinks->output_item_done == NULL)
    return CAI_OK;
  CAI_LJ->spooled_init(CAI_LJ, &item);
  memset(&writer, 0, sizeof(writer));
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_chat_spool_append,
                                    &item, &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = writer.begin_object(&writer, &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = cai_chat_member(&writer, "id", "chat_text", &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = cai_chat_member(&writer, "type", "message", &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = cai_chat_member(&writer, "role", "assistant", &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = cai_chat_member(&writer, "status", "completed", &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = cai_chat_key(&writer, "content", &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = writer.begin_array(&writer, &json_error);
  for (part = 0; status == LONEJSON_STATUS_OK && part < 2; part++) {
    if (!(part == 0 ? state->text_started : state->refusal_started))
      continue;
    status = writer.begin_object(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = cai_chat_member(
          &writer, "type", part == 0 ? "output_text" : "refusal", &json_error);
    if (status == LONEJSON_STATUS_OK)
      status =
          cai_chat_key(&writer, part == 0 ? "text" : "refusal", &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = writer.string_spooled(
          &writer, part == 0 ? &state->text : &state->refusal, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK)
    status = writer.end_array(&writer, &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = writer.end_object(&writer, &json_error);
  if (status == LONEJSON_STATUS_OK)
    status = writer.finish(&writer, &json_error);
  if (writer.cleanup != NULL)
    writer.cleanup(&writer);
  rc = status == LONEJSON_STATUS_OK
           ? state->sinks->output_item_done(state->sinks->output_item_context,
                                            "chat_text", 0, "message", &item,
                                            state->error)
           : cai_set_error_detail(state->error, CAI_ERR_PROTOCOL,
                                  "failed to serialize Chat message",
                                  json_error.message);
  item.cleanup(&item);
  return rc;
}

static int cai_chat_emit_tool_done(cai_chat_stream_state *state, size_t index) {
  cai_chat_tool_state *tool;
  cai_chat_custom_arguments_doc custom;
  cai_chat_reader reader;
  lonejson_spooled item;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  tool = &state->tools[index];
  if (tool->id == NULL || tool->name == NULL) {
    return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "Chat Completions tool call lacks id or name");
  }
  if (!tool->has_arguments) {
    CAI_LJ->spooled_init(CAI_LJ, &tool->arguments);
    tool->has_arguments = 1;
    lonejson_error_init(&json_error);
    if (tool->arguments.append(&tool->arguments, "{}", 2U, &json_error) !=
        LONEJSON_STATUS_OK) {
      return cai_set_error(state->error, CAI_ERR_TRANSPORT,
                           "failed to retain empty tool arguments");
    }
  }
  memset(&custom, 0, sizeof(custom));
  if (tool->is_custom) {
    if (tool->custom != NULL) {
      rc = cai_chat_custom_finish(tool->custom);
      if (rc != CAI_OK)
        return rc;
    }
    memset(&reader, 0, sizeof(reader));
    reader.use_spool = 1;
    reader.spool = tool->arguments;
    lonejson_error_init(&json_error);
    if (reader.spool.rewind(&reader.spool, &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                           "failed to read custom tool arguments");
    }
    CAI_LJ->init(CAI_LJ, &cai_chat_custom_arguments_map, &custom);
    if (CAI_LJ->parse_reader(CAI_LJ, &cai_chat_custom_arguments_map, &custom,
                             cai_chat_read, &reader,
                             &json_error) != LONEJSON_STATUS_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_chat_custom_arguments_map, &custom);
      return cai_set_error_detail(state->error, CAI_ERR_PROTOCOL,
                                  "invalid custom tool arguments",
                                  json_error.message);
    }
    if (state->sinks->custom_tool_call_input_done != NULL) {
      rc = state->sinks->custom_tool_call_input_done(
          state->sinks->custom_tool_call_context, tool->id, (int)index + 1,
          tool->id, tool->name, &custom.input, state->error);
      if (rc != CAI_OK) {
        CAI_LJ->cleanup(CAI_LJ, &cai_chat_custom_arguments_map, &custom);
        return rc;
      }
    }
  } else if (state->sinks->function_call_arguments_done != NULL) {
    rc = state->sinks->function_call_arguments_done(
        state->sinks->function_call_context, tool->id, (int)index + 1, tool->id,
        tool->name, &tool->arguments, state->error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (state->sinks->output_item_done == NULL) {
    if (tool->is_custom) {
      CAI_LJ->cleanup(CAI_LJ, &cai_chat_custom_arguments_map, &custom);
    }
    return CAI_OK;
  }
  CAI_LJ->spooled_init(CAI_LJ, &item);
  memset(&writer, 0, sizeof(writer));
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_chat_spool_append,
                                    &item, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "id", tool->id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(
        &writer, "type", tool->is_custom ? "custom_tool_call" : "function_call",
        &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "call_id", tool->id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "name", tool->name, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, tool->is_custom ? "input" : "arguments",
                          &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string_spooled(
        &writer, tool->is_custom ? &custom.input : &tool->arguments,
        &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer.cleanup != NULL) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    item.cleanup(&item);
    if (tool->is_custom) {
      CAI_LJ->cleanup(CAI_LJ, &cai_chat_custom_arguments_map, &custom);
    }
    return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "failed to serialize Chat Completions tool call");
  }
  rc = state->sinks->output_item_done(
      state->sinks->output_item_context, tool->id, (int)index + 1,
      tool->is_custom ? "custom_tool_call" : "function_call", &item,
      state->error);
  item.cleanup(&item);
  if (tool->is_custom) {
    CAI_LJ->cleanup(CAI_LJ, &cai_chat_custom_arguments_map, &custom);
  }
  return rc;
}

static int cai_chat_parse_event(cai_chat_stream_state *state) {
  cai_chat_reader reader;
  cai_chat_chunk_doc chunk;
  cai_chat_chunk_choice_doc *choices;
  cai_chat_chunk_tool_doc *tools;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  size_t j;
  int rc;

  memset(&reader, 0, sizeof(reader));
  reader.spool = state->event;
  reader.use_spool = 1;
  lonejson_error_init(&json_error);
  if (reader.spool.rewind(&reader.spool, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "failed to rewind Chat Completions SSE event");
  }
  memset(&chunk, 0, sizeof(chunk));
  CAI_LJ->init(CAI_LJ, &cai_chat_chunk_map, &chunk);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_chat_chunk_map, &chunk,
                                cai_chat_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_chat_chunk_map, &chunk);
    return cai_set_error_detail(state->error, CAI_ERR_PROTOCOL,
                                "invalid Chat Completions SSE chunk",
                                json_error.message);
  }
  rc = CAI_OK;
  if (chunk.error.message != NULL || chunk.error.code != NULL ||
      chunk.error.type != NULL) {
    rc = cai_set_error_http(
        state->error, CAI_ERR_SERVER, state->http_status,
        "Chat Completions inference failed", chunk.error.message,
        chunk.error.code != NULL ? chunk.error.code : chunk.error.type,
        state->request_id != NULL ? *state->request_id : NULL);
  }
  if (rc == CAI_OK && state->completed && chunk.choices.count > 0U) {
    rc = cai_set_error(state->error, CAI_ERR_PROTOCOL,
                       "Chat Completions choice arrived after finish_reason");
  }
  if (rc == CAI_OK && chunk.id != NULL && state->out_response_id != NULL &&
      *state->out_response_id == NULL) {
    *state->out_response_id = cai_strdup(NULL, chunk.id);
    if (*state->out_response_id == NULL) {
      rc = cai_set_error(state->error, CAI_ERR_NOMEM,
                         "failed to retain Chat Completions response id");
    }
  }
  choices = (cai_chat_chunk_choice_doc *)chunk.choices.items;
  if (rc == CAI_OK && chunk.choices.count != 0U)
    state->model_activity = 1;
  for (i = 0U; rc == CAI_OK && i < chunk.choices.count; i++) {
    if (state->first_token_ms < 0.0 &&
        (choices[i].delta.content.size_fn(&choices[i].delta.content) > 0U ||
         choices[i].delta.reasoning_content.size_fn(
             &choices[i].delta.reasoning_content) > 0U ||
         choices[i].delta.refusal.size_fn(&choices[i].delta.refusal) > 0U ||
         choices[i].delta.tool_calls.count > 0U))
      state->first_token_ms = (cai_chat_now() - state->started_at) * 1000.0;
    if (choices[i].index != 0LL) {
      rc = cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "unexpected Chat Completions choice index");
      break;
    }
    rc = cai_chat_emit_text(state, &choices[i].delta.reasoning_content,
                            CAI_CHAT_REASONING);
    if (state->glm_thinking && choices[i].delta.reasoning_content.size_fn(
                                   &choices[i].delta.reasoning_content) > 0U) {
      state->glm_thinking_done = 1;
    }
    if (rc == CAI_OK) {
      rc = state->glm_thinking && choices[i].delta.reasoning_content.size_fn(
                                      &choices[i].delta.reasoning_content) == 0U
               ? cai_chat_emit_glm_content(state, &choices[i].delta.content)
               : cai_chat_emit_text(state, &choices[i].delta.content,
                                    CAI_CHAT_TEXT);
    }
    if (rc == CAI_OK)
      rc = cai_chat_emit_text(state, &choices[i].delta.refusal,
                              CAI_CHAT_REFUSAL);
    tools = (cai_chat_chunk_tool_doc *)choices[i].delta.tool_calls.items;
    for (j = 0U; rc == CAI_OK && j < choices[i].delta.tool_calls.count; j++) {
      rc = cai_chat_emit_tool_delta(state, &tools[j]);
    }
    if (rc == CAI_OK && choices[i].finish_reason != NULL) {
      if (strcmp(choices[i].finish_reason, "length") == 0) {
        state->length_finish = 1;
        state->completed = 1;
      } else if (state->glm_thinking && state->glm_thinking_started &&
                 !state->glm_thinking_done) {
        rc = cai_set_error(state->error, CAI_ERR_PROTOCOL,
                           "GLM ended before its thinking delimiter; final "
                           "answer was not available");
        break;
      } else if (strcmp(choices[i].finish_reason, "stop") == 0 ||
                 strcmp(choices[i].finish_reason, "tool_calls") == 0) {
        state->completed = 1;
      } else {
        rc = cai_set_error_detail(state->error, CAI_ERR_PROTOCOL,
                                  "Chat Completions ended unexpectedly",
                                  choices[i].finish_reason);
      }
    }
  }
  if (rc == CAI_OK)
    rc = cai_chat_flush_glm_prefix(state);
  if (rc == CAI_OK && chunk.usage.total_tokens > 0LL) {
    state->usage.input_tokens = chunk.usage.prompt_tokens;
    state->usage.input_cached_tokens =
        chunk.usage.prompt_tokens_details.cached_tokens;
    state->usage.output_tokens = chunk.usage.completion_tokens;
    state->usage.output_reasoning_tokens =
        chunk.usage.completion_tokens_details.reasoning_tokens;
    state->usage.total_tokens = chunk.usage.total_tokens;
    if (state->out_usage != NULL) {
      *state->out_usage = state->usage;
    }
    state->saw_usage = 1;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_chat_chunk_map, &chunk);
  return rc;
}

static lonejson_status cai_chat_sse_data(void *user, const void *bytes,
                                         size_t count, lonejson_error *error) {
  cai_chat_stream_state *state;

  state = (cai_chat_stream_state *)user;
  return state->event.append(&state->event, bytes, count, error);
}

static lonejson_status cai_chat_sse_end(void *user,
                                        const lonejson_sse_event *event,
                                        lonejson_error *error) {
  cai_chat_stream_state *state;
  lonejson_spooled cursor;
  lonejson_read_result read_result;
  char done[6];
  int rc;

  (void)event;
  (void)error;
  state = (cai_chat_stream_state *)user;
  if (state->event.size_fn(&state->event) == 0U) {
    return LONEJSON_STATUS_OK;
  }
  if (state->done_seen) {
    state->failure_code =
        cai_set_error(state->error, CAI_ERR_PROTOCOL,
                      "Chat Completions data arrived after [DONE]");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  cursor = state->event;
  if (cursor.rewind(&cursor, error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  read_result = cursor.read(&cursor, (unsigned char *)done, sizeof(done));
  if (read_result.error_code != 0) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  if (state->event.size_fn(&state->event) == 6U &&
      read_result.bytes_read == 6U && memcmp(done, "[DONE]", 6U) == 0) {
    state->done_seen = 1;
    state->event.reset(&state->event);
    return LONEJSON_STATUS_OK;
  }
  rc = cai_chat_parse_event(state);
  state->event.reset(&state->event);
  if (rc != CAI_OK) {
    state->failure_code = rc;
    if (state->error != NULL && state->error->code == CAI_OK) {
      cai_set_error(state->error, rc,
                    "Chat Completions stream callback failed");
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static size_t cai_chat_curl_write(char *ptr, size_t size, size_t nmemb,
                                  void *user) {
  cai_chat_stream_state *state;
  lonejson_sse_handler handler;
  lonejson_error json_error;
  lonejson_status status;
  size_t count;
  size_t copy;
  char *grown;

  state = (cai_chat_stream_state *)user;
  count = size * nmemb;
  state->response_bytes += count;
  copy = count;
  if (copy > 65536U - state->body_length) {
    copy = 65536U - state->body_length;
  }
  if (copy > 0U) {
    grown = (char *)cai_realloc_mem(NULL, state->body,
                                    state->body_length + copy + 1U);
    if (grown == NULL) {
      cai_set_error(state->error, CAI_ERR_NOMEM,
                    "failed to retain Chat Completions error body");
      state->failure_code = CAI_ERR_NOMEM;
      return 0U;
    }
    state->body = grown;
    memcpy(state->body + state->body_length, ptr, copy);
    state->body_length += copy;
    state->body[state->body_length] = '\0';
  }
  memset(&handler, 0, sizeof(handler));
  handler.data_chunk = cai_chat_sse_data;
  handler.end_event = cai_chat_sse_end;
  lonejson_error_init(&json_error);
  status =
      lonejson_sse_push(state->sse, ptr, count, &handler, state, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    if (state->failure_code == CAI_OK) {
      state->failure_code = CAI_ERR_PROTOCOL;
    }
    if (state->error == NULL || state->error->code == CAI_OK) {
      cai_set_error_detail(state->error, CAI_ERR_PROTOCOL,
                           "invalid Chat Completions SSE stream",
                           json_error.message);
    }
    return 0U;
  }
  return count;
}

static size_t cai_chat_curl_header(char *ptr, size_t size, size_t nmemb,
                                   void *user) {
  cai_chat_stream_state *state = (cai_chat_stream_state *)user;
  size_t count = size * nmemb;
  long status;
  char line[64];

  if (count >= 5U && memcmp(ptr, "HTTP/", 5U) == 0) {
    size_t copy = count < sizeof(line) - 1U ? count : sizeof(line) - 1U;
    memcpy(line, ptr, copy);
    line[copy] = '\0';
    if (sscanf(line, "HTTP/%*s %ld", &status) == 1) {
      state->http_status = status;
      state->retry_after_ms = -1L;
    }
  }
  if (count > 12U && strncasecmp(ptr, "Retry-After:", 12U) == 0) {
    char value[128];
    size_t len = count - 12U;
    if (len < sizeof(value)) {
      char *start;
      char *end;
      unsigned long seconds;
      time_t date;
      memcpy(value, ptr + 12U, len);
      value[len] = '\0';
      start = value;
      while (*start == ' ' || *start == '\t')
        start++;
      end = value + len;
      while (end > start && (end[-1] == '\r' || end[-1] == '\n' ||
                             end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
      if (*start >= '0' && *start <= '9') {
        errno = 0;
        seconds = strtoul(start, &end, 10);
        if (*end == '\0')
          state->retry_after_ms =
              errno == ERANGE || seconds > (unsigned long)LONG_MAX / 1000UL
                  ? LONG_MAX
                  : (long)seconds * 1000L;
      } else if ((date = curl_getdate(start, NULL)) != (time_t)-1) {
        double remaining = difftime(date, time(NULL));
        state->retry_after_ms = remaining <= 0.0 ? 0L
                                : remaining > (double)LONG_MAX / 1000.0
                                    ? LONG_MAX
                                    : (long)(remaining * 1000.0);
      }
    } else {
      /* Oversized values must never make the client retry earlier. */
      state->retry_after_ms = LONG_MAX;
    }
  }
  return cai_http_request_id_header_write(ptr, size, nmemb, state->request_id);
}

static int cai_chat_cancel_progress(void *context, curl_off_t down_total,
                                    curl_off_t down_now, curl_off_t up_total,
                                    curl_off_t up_now) {
  (void)context;
  (void)down_total;
  (void)down_now;
  (void)up_total;
  (void)up_now;
  return cai_stream_thread_cancel_requested();
}

static void cai_chat_stream_cleanup(cai_chat_stream_state *state) {
  size_t i;

  for (i = 0U; i < state->tool_count; i++) {
    cai_chat_custom_close(state->tools[i].custom);
    cai_free_mem(NULL, state->tools[i].id);
    cai_free_mem(NULL, state->tools[i].name);
    if (state->tools[i].has_arguments) {
      state->tools[i].arguments.cleanup(&state->tools[i].arguments);
    }
  }
  cai_free_mem(NULL, state->tools);
  if (state->event.cleanup != NULL) {
    state->event.cleanup(&state->event);
  }
  if (state->text.cleanup != NULL)
    state->text.cleanup(&state->text);
  if (state->refusal.cleanup != NULL)
    state->refusal.cleanup(&state->refusal);
  if (state->sse != NULL) {
    lonejson_sse_close(state->sse);
  }
  cai_free_mem(NULL, state->body);
}

typedef struct cai_chat_attempt_result {
  long status;
  long retry_after_ms;
  int retryable;
} cai_chat_attempt_result;

static int cai_chat_stream_attempt(
    cai_client *client, const cai_response_create_params *params,
    const cai_stream_sinks *sinks, char **out_response_id,
    cai_token_usage *out_usage, cai_error *error, unsigned int attempt,
    cai_chat_attempt_result *result, size_t capture_limit_bytes) {
  cai_client_impl *impl;
  cai_chat_stream_state state;
  cai_chat_upload *upload;
  struct curl_slist *headers;
  CURL *curl;
  CURLcode curl_rc;
  lonejson_sse_options options;
  lonejson_sse_handler handler;
  lonejson_error json_error;
  char *url;
  char *request_id;
  char curl_error[CURL_ERROR_SIZE];
  long http_status;
  size_t i;
  int rc;
  int upload_rc;
  int upload_pipe_closed;
  cai_error upload_error;

  if (client == NULL || params == NULL || sinks == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, params and stream sinks are required");
  }
  impl = CAI_CLIENT_IMPL(client);
  if (sinks->output_text == NULL && sinks->reasoning_summary == NULL &&
      sinks->output_text_delta == NULL && sinks->output_item_done == NULL &&
      sinks->function_call_arguments_delta == NULL &&
      sinks->function_call_arguments_done == NULL &&
      sinks->custom_tool_call_input_delta == NULL &&
      sinks->custom_tool_call_input_done == NULL &&
      sinks->response_completed == NULL && sinks->compaction_progress == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "at least one stream sink or callback is required");
  }
  memset(&state, 0, sizeof(state));
  memset(result, 0, sizeof(*result));
  state.started_at = cai_chat_now();
  state.first_token_ms = -1.0;
  state.retry_after_ms = -1L;
  state.client = client;
  state.params = params;
  state.sinks = sinks;
  state.error = error;
  state.out_response_id = out_response_id;
  state.out_usage = out_usage;
  state.capture_limit_bytes = capture_limit_bytes;
  state.glm_thinking = params->text_format_schema_json == NULL &&
                       (params->text_format_type == NULL ||
                        strcmp(params->text_format_type, "json_object") != 0) &&
                       params->model != NULL &&
                       (strcmp(params->model, "zai-org/GLM-5.3-Flash") == 0 ||
                        strcmp(params->model, "glm-5.3-flash") == 0 ||
                        strcmp(params->model, "glm/5.3-flash") == 0 ||
                        strcmp(params->model, "glm5.3-flash") == 0 ||
                        strcmp(params->model, "zai/glm-5.3-flash") == 0);
  options = lonejson_default_sse_options();
  options.max_line_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  options.max_event_data_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  options.max_buffered_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  lonejson_error_init(&json_error);
  state.sse = lonejson_sse_open(&options, &json_error);
  if (state.sse == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to open Chat Completions SSE parser");
  }
  CAI_LJ->spooled_init(CAI_LJ, &state.event);
  if (sinks->output_item_done != NULL) {
    CAI_LJ->spooled_init(CAI_LJ, &state.text);
    CAI_LJ->spooled_init(CAI_LJ, &state.refusal);
  }
  upload = NULL;
  headers = NULL;
  url = NULL;
  request_id = NULL;
  state.request_id = &request_id;
  curl_error[0] = '\0';
  curl = NULL;
  rc = cai_chat_upload_open(params, &upload, error);
  if (rc == CAI_OK) {
    rc = cai_build_url(&impl->allocator, impl->base_url, "chat/completions",
                       &url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Content-Type: application/json", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Accept: text/event-stream", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Expect:", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_bearer_header(client, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_client_headers(client, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Organization: ",
                                    impl->organization_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(
        client, &headers, "OpenAI-Project: ", impl->project_id, error);
  }
  if (rc == CAI_OK) {
    curl = curl_easy_init();
    if (curl == NULL) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize Chat Completions HTTP client");
    }
  }
  curl_rc = CURLE_OK;
  http_status = 0L;
  upload_rc = CAI_OK;
  upload_pipe_closed = 0;
  cai_error_init(&upload_error);
  if (rc == CAI_OK) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, cai_chat_upload_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     cai_chat_upload_size(upload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_chat_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_chat_curl_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cai_chat_cancel_progress);
    if (impl->timeout_ms > 0L) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
    }
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     (long)(impl->http_2_disabled ? CURL_HTTP_VERSION_1_1
                                                  : CURL_HTTP_VERSION_2TLS));
    cai_configure_curl_tls(curl, impl->insecure_skip_verify,
                           impl->ca_bundle_path, impl->ca_path);
    cai_log_http_request_start(impl, "POST", "chat/completions", 1,
                               (size_t)cai_chat_upload_size(upload));
    curl_rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (curl_rc == CURLE_OK) {
      cai_log_http_request_done(impl, "POST", "chat/completions", http_status,
                                state.response_bytes, request_id);
    }
  }
  if (curl != NULL) {
    curl_easy_cleanup(curl);
  }
  if (upload != NULL) {
    upload_rc =
        cai_chat_upload_close(upload, &upload_pipe_closed, &upload_error);
  }
  if (rc == CAI_OK && curl_rc == CURLE_OK && http_status >= 200L &&
      http_status < 300L && state.failure_code == CAI_OK) {
    memset(&handler, 0, sizeof(handler));
    handler.data_chunk = cai_chat_sse_data;
    handler.end_event = cai_chat_sse_end;
    lonejson_error_init(&json_error);
    if (lonejson_sse_finish(state.sse, &handler, &state, &json_error) !=
        LONEJSON_STATUS_OK) {
      rc = state.failure_code != CAI_OK
               ? state.failure_code
               : cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                      "invalid Chat Completions SSE ending",
                                      json_error.message);
    }
  }
  if (rc == CAI_OK && http_status != 0L &&
      (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status,
                              state.body != NULL ? state.body : "", request_id);
  } else if (rc == CAI_OK && curl_rc != CURLE_OK) {
    if (cai_stream_thread_cancel_requested()) {
      rc = cai_set_error(error, CAI_ERR_CANCELLED, "agent turn was cancelled");
    } else if (state.failure_code != CAI_OK) {
      rc = state.failure_code;
    } else {
      rc = cai_set_error_http(
          error, CAI_ERR_TRANSPORT, http_status,
          "Chat Completions HTTP request failed",
          curl_error[0] != '\0' ? curl_error : curl_easy_strerror(curl_rc),
          NULL, request_id);
      cai_log_http_transport_error(
          impl, "POST", "chat/completions",
          curl_error[0] != '\0' ? curl_error : curl_easy_strerror(curl_rc));
    }
  }
  if (rc == CAI_OK && upload_rc != CAI_OK) {
    rc = cai_set_error_detail(error, upload_rc,
                              "Chat Completions request upload failed",
                              upload_error.message);
  }
  if (rc == CAI_OK && !state.completed) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chat Completions stream ended without finish_reason");
  }
  if (rc == CAI_OK && !state.done_seen) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chat Completions stream ended before [DONE]");
  }
  if (rc == CAI_OK && state.visible_started) {
    rc = cai_chat_emit_affix(sinks->output_text, &sinks->output_text_suffix,
                             error);
  }
  if (rc == CAI_OK)
    rc = cai_chat_finish_reasoning(&state);
  if (rc == CAI_OK && state.length_finish) {
    long long window = cai_client_model_context_window(client, params->model);
    int pressure = window > 0LL && state.saw_usage &&
                   state.usage.total_tokens >= (window * 9LL) / 10LL;
    rc = cai_set_error_http(
        error, CAI_ERR_LIMIT, 0L,
        pressure ? "Chat Completions context limit reached"
                 : "Chat Completions output token limit reached",
        pressure ? "finish_reason=length near model context window"
                 : "finish_reason=length; continuation is required",
        pressure ? "context_length_exceeded" : "output_length", request_id);
  }
  if (rc == CAI_OK)
    rc = cai_chat_emit_message_done(&state);
  for (i = 0U; rc == CAI_OK && i < state.tool_count; i++) {
    rc = cai_chat_emit_tool_done(&state, i);
  }
  cai_log_chat_result(impl, params->model, rc, http_status, request_id, error);
  cai_log_chat_metrics(client, params->model, request_id, attempt,
                       (cai_chat_now() - state.started_at) * 1000.0,
                       state.first_token_ms, state.saw_usage, &state.usage);
  result->status = http_status;
  result->retry_after_ms = state.retry_after_ms;
  result->retryable =
      !state.model_activity && !state.saw_usage &&
      !cai_stream_thread_cancel_requested() &&
      /* Cleanup shuts the pipe while its producer may still be writing.
       * That is a consequence of the transport failure, not bad input. */
      (upload_rc == CAI_OK || upload_pipe_closed) &&
      state.failure_code == CAI_OK &&
      ((rc == CAI_ERR_SERVER &&
        (http_status == 429L || http_status == 502L || http_status == 503L)) ||
       (rc == CAI_ERR_TRANSPORT && http_status == 0L &&
        (curl_rc == CURLE_COULDNT_CONNECT ||
         curl_rc == CURLE_OPERATION_TIMEDOUT || curl_rc == CURLE_GOT_NOTHING ||
         curl_rc == CURLE_SEND_ERROR || curl_rc == CURLE_RECV_ERROR)));
  if (error != NULL && error->server_code != NULL &&
      (strcmp(error->server_code, "insufficient_quota") == 0 ||
       strcmp(error->server_code, "quota_exceeded") == 0 ||
       strcmp(error->server_code, "billing_hard_limit_reached") == 0))
    result->retryable = 0;
  curl_slist_free_all(headers);
  cai_free_mem(&impl->allocator, url);
  cai_free_mem(NULL, request_id);
  cai_chat_stream_cleanup(&state);
  cai_error_cleanup(&upload_error);
  return rc;
}

static int cai_chat_stream_retries(cai_client *client,
                                   const cai_response_create_params *params,
                                   const cai_stream_sinks *sinks,
                                   char **out_response_id,
                                   cai_token_usage *out_usage, cai_error *error,
                                   size_t capture_limit_bytes) {
  cai_chat_attempt_result result;
  cai_client_impl *impl;
  unsigned int attempt;
  long backoff;
  int rc;
  if (client == NULL || params == NULL || sinks == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, params and stream sinks are required");
  impl = CAI_CLIENT_IMPL(client);
  backoff = impl->chat_retry_initial_delay_ms;
  for (attempt = 1U;; attempt++) {
    long delay;
    long remaining;
    struct timespec jitter = {0, 0};
    if (cai_stream_thread_cancel_requested())
      return cai_set_error(error, CAI_ERR_CANCELLED,
                           "agent turn was cancelled");
    memset(&result, 0, sizeof(result));
    rc = cai_chat_stream_attempt(client, params, sinks, out_response_id,
                                 out_usage, error, attempt, &result,
                                 capture_limit_bytes);
    if (!result.retryable || attempt >= impl->chat_max_attempts)
      return rc;
    /* Clock nanoseconds provide request-local jitter without process-global
     * random state. Retry-After is a minimum, including when it exceeds our
     * cap. */
    (void)clock_gettime(CLOCK_MONOTONIC, &jitter);
    delay = backoff - backoff / 4L + jitter.tv_nsec % (backoff / 2L + 1L);
    if (delay > impl->chat_retry_max_delay_ms)
      delay = impl->chat_retry_max_delay_ms;
    if (result.retry_after_ms > impl->chat_retry_max_delay_ms)
      return rc;
    if (result.retry_after_ms > delay)
      delay = result.retry_after_ms;
    cai_log_chat_retry(impl, attempt + 1U, result.status, delay);
    remaining = delay;
    while (remaining > 0L) {
      long chunk = remaining > 10L ? 10L : remaining;
      struct timespec pause;
      pause.tv_sec = 0;
      pause.tv_nsec = chunk * 1000000L;
      if (cai_stream_thread_cancel_requested())
        return cai_set_error(error, CAI_ERR_CANCELLED,
                             "agent turn was cancelled during retry wait");
      while (cai_chat_retry_sleep(&pause, &pause) != 0) {
        if (errno != EINTR)
          return cai_set_error(error, CAI_ERR_TRANSPORT,
                               "Chat retry wait failed");
        if (cai_stream_thread_cancel_requested())
          return cai_set_error(error, CAI_ERR_CANCELLED,
                               "agent turn was cancelled during retry wait");
      }
      remaining -= chunk;
    }
    cai_error_cleanup(error);
    if (backoff > impl->chat_retry_max_delay_ms / 2L)
      backoff = impl->chat_retry_max_delay_ms;
    else
      backoff *= 2L;
  }
}

int cai_chat_stream_response(cai_client *client,
                             const cai_response_create_params *params,
                             const cai_stream_sinks *sinks,
                             char **out_response_id, cai_token_usage *out_usage,
                             cai_error *error) {
  cai_error local_error;
  int rc;
  cai_error_init(&local_error);
  rc =
      cai_chat_stream_retries(client, params, sinks, out_response_id, out_usage,
                              error != NULL ? error : &local_error, 0U);
  cai_error_cleanup(&local_error);
  return rc;
}

typedef struct cai_chat_response_capture {
  lonejson_spooled items;
  lonejson_writer items_writer;
} cai_chat_response_capture;

static int cai_chat_capture_item(void *user, const char *item_id,
                                 int output_index, const char *type,
                                 const lonejson_spooled *item,
                                 cai_error *error) {
  cai_chat_response_capture *capture;
  lonejson_error json_error;

  (void)item_id;
  (void)output_index;
  (void)type;
  capture = (cai_chat_response_capture *)user;
  lonejson_error_init(&json_error);
  if (capture->items_writer.json_value_spooled(
          &capture->items_writer, item, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to retain Chat Completions output item",
                                json_error.message);
  }
  return CAI_OK;
}

typedef struct cai_chat_buffer_sink_context {
  cai_buffer_builder *builder;
  cai_error *error;
  size_t limit;
  int failure_code;
} cai_chat_buffer_sink_context;

static lonejson_status cai_chat_buffer_sink(void *user, const void *bytes,
                                            size_t count,
                                            lonejson_error *json_error) {
  cai_chat_buffer_sink_context *context;

  (void)json_error;
  context = (cai_chat_buffer_sink_context *)user;
  if (count > context->limit - context->builder->length) {
    context->failure_code = cai_set_error(
        context->error, CAI_ERR_LIMIT,
        "Chat Completions response exceeded configured JSON response limit");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  context->failure_code = cai_buffer_append(
      context->builder, (const char *)bytes, count, context->error);
  return context->failure_code == CAI_OK ? LONEJSON_STATUS_OK
                                         : LONEJSON_STATUS_CALLBACK_FAILED;
}

static int
cai_chat_capture_build_response(cai_client *client,
                                const cai_response_create_params *params,
                                const char *id, const cai_token_usage *usage,
                                const cai_chat_response_capture *capture,
                                cai_response **out, cai_error *error) {
  cai_buffer_builder builder;
  cai_chat_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  memset(&builder, 0, sizeof(builder));
  builder.allocator = &CAI_CLIENT_IMPL(client)->allocator;
  sink_context.builder = &builder;
  sink_context.error = error;
  sink_context.limit = CAI_CLIENT_IMPL(client)->json_response_limit_bytes;
  sink_context.failure_code = CAI_OK;
  memset(&writer, 0, sizeof(writer));
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_chat_buffer_sink,
                                    &sink_context, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "id", id != NULL ? id : "chat_response",
                             &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "status", "completed", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_member(&writer, "model", params->model, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "output", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status =
        writer.array_items_spooled(&writer, "", &capture->items, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "usage", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "input_tokens", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.i64(&writer, usage->input_tokens, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "input_tokens_details", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "cached_tokens", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.i64(&writer, usage->input_cached_tokens, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "output_tokens", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.i64(&writer, usage->output_tokens, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "output_tokens_details", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "reasoning_tokens", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.i64(&writer, usage->output_reasoning_tokens, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_chat_key(&writer, "total_tokens", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.i64(&writer, usage->total_tokens, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer.cleanup != NULL) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(builder.allocator, builder.data);
    if (sink_context.failure_code != CAI_OK)
      return sink_context.failure_code;
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to build Chat Completions response",
                                json_error.message);
  }
  rc = cai_response_parse_json_with_allocator(
      &CAI_CLIENT_IMPL(client)->allocator, builder.data, out, error);
  cai_free_mem(builder.allocator, builder.data);
  return rc;
}

int cai_chat_create_response(cai_client *client,
                             const cai_response_create_params *params,
                             cai_response **out, cai_error *error) {
  cai_chat_response_capture capture;
  cai_stream_sinks sinks;
  cai_token_usage usage;
  cai_error local_error;
  lonejson_error json_error;
  lonejson_status status;
  char *id;
  int rc;

  if (client == NULL || params == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chat Completions client, params and output "
                         "are required");
  }
  *out = NULL;
  cai_error_init(&local_error);
  if (error == NULL)
    error = &local_error;
  memset(&capture, 0, sizeof(capture));
  CAI_LJ->spooled_init(CAI_LJ, &capture.items);
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &capture.items_writer,
                                    cai_chat_spool_append, &capture.items,
                                    &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status =
        capture.items_writer.begin_array(&capture.items_writer, &json_error);
  }
  rc = status == LONEJSON_STATUS_OK
           ? CAI_OK
           : cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to initialize Chat response capture",
                                  json_error.message);
  id = NULL;
  memset(&usage, 0, sizeof(usage));
  if (rc == CAI_OK) {
    cai_stream_sinks_init(&sinks);
    sinks.output_item_done = cai_chat_capture_item;
    sinks.output_item_context = &capture;
    rc = cai_chat_stream_retries(
        client, params, &sinks, &id, &usage, error,
        CAI_CLIENT_IMPL(client)->json_response_limit_bytes);
  }
  if (rc == CAI_OK) {
    status = capture.items_writer.end_array(&capture.items_writer, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = capture.items_writer.finish(&capture.items_writer, &json_error);
    }
    if (status != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to finish Chat response capture",
                                json_error.message);
    }
  }
  if (capture.items_writer.cleanup != NULL) {
    capture.items_writer.cleanup(&capture.items_writer);
  }
  if (rc == CAI_OK) {
    rc = cai_chat_capture_build_response(client, params, id, &usage, &capture,
                                         out, error);
  }
  cai_free_mem(NULL, id);
  capture.items.cleanup(&capture.items);
  if (rc == CAI_ERR_LIMIT)
    cai_log_http_response_limit(
        CAI_CLIENT_IMPL(client), "POST", "chat/completions",
        CAI_CLIENT_IMPL(client)->json_response_limit_bytes);
  cai_error_cleanup(&local_error);
  return rc;
}
