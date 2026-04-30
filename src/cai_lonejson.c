#include "cai_internal.h"

#include <string.h>

typedef struct cai_spooled_append_doc {
  lonejson_spooled value;
} cai_spooled_append_doc;

typedef struct cai_spooled_append_reader {
  const unsigned char *prefix;
  size_t prefix_len;
  size_t prefix_pos;
  lonejson_spooled old_cursor;
  int has_old;
  const unsigned char *data;
  size_t data_len;
  size_t data_pos;
  const unsigned char *suffix;
  size_t suffix_len;
  size_t suffix_pos;
  unsigned char escape[6];
  size_t escape_len;
  size_t escape_pos;
  int phase;
  int failed;
} cai_spooled_append_reader;

static const char cai_hex_digits[] = "0123456789abcdef";

static const lonejson_field cai_spooled_append_fields[] = {
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_spooled_append_doc, value, "value")};

static void cai_spooled_append_escape_byte(cai_spooled_append_reader *reader,
                                           unsigned char ch) {
  reader->escape_pos = 0U;
  if (ch == (unsigned char)'"' || ch == (unsigned char)'\\') {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = ch;
    reader->escape_len = 2U;
  } else if (ch == (unsigned char)'\b') {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = (unsigned char)'b';
    reader->escape_len = 2U;
  } else if (ch == (unsigned char)'\f') {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = (unsigned char)'f';
    reader->escape_len = 2U;
  } else if (ch == (unsigned char)'\n') {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = (unsigned char)'n';
    reader->escape_len = 2U;
  } else if (ch == (unsigned char)'\r') {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = (unsigned char)'r';
    reader->escape_len = 2U;
  } else if (ch == (unsigned char)'\t') {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = (unsigned char)'t';
    reader->escape_len = 2U;
  } else if (ch < (unsigned char)0x20) {
    reader->escape[0] = (unsigned char)'\\';
    reader->escape[1] = (unsigned char)'u';
    reader->escape[2] = (unsigned char)'0';
    reader->escape[3] = (unsigned char)'0';
    reader->escape[4] = (unsigned char)cai_hex_digits[(ch >> 4U) & 0x0FU];
    reader->escape[5] = (unsigned char)cai_hex_digits[ch & 0x0FU];
    reader->escape_len = 6U;
  } else {
    reader->escape[0] = ch;
    reader->escape_len = 1U;
  }
}

static int cai_spooled_append_next_byte(cai_spooled_append_reader *reader,
                                        unsigned char *out) {
  lonejson_read_result result;
  unsigned char ch;

  if (reader->escape_pos < reader->escape_len) {
    *out = reader->escape[reader->escape_pos++];
    return 1;
  }
  reader->escape_len = 0U;
  reader->escape_pos = 0U;
  for (;;) {
    if (reader->phase == 0) {
      if (reader->prefix_pos < reader->prefix_len) {
        *out = reader->prefix[reader->prefix_pos++];
        return 1;
      }
      reader->phase = 1;
    } else if (reader->phase == 1) {
      if (!reader->has_old) {
        reader->phase = 2;
      } else {
        result = lonejson_spooled_read(&reader->old_cursor, &ch, 1U);
        if (result.error_code != 0) {
          reader->failed = 1;
          return 0;
        }
        if (result.bytes_read > 0U) {
          cai_spooled_append_escape_byte(reader, ch);
          *out = reader->escape[reader->escape_pos++];
          return 1;
        }
        if (result.eof) {
          reader->phase = 2;
        }
      }
    } else if (reader->phase == 2) {
      if (reader->data_pos < reader->data_len) {
        cai_spooled_append_escape_byte(reader, reader->data[reader->data_pos++]);
        *out = reader->escape[reader->escape_pos++];
        return 1;
      }
      reader->phase = 3;
    } else if (reader->phase == 3) {
      if (reader->suffix_pos < reader->suffix_len) {
        *out = reader->suffix[reader->suffix_pos++];
        return 1;
      }
      reader->phase = 4;
    } else {
      return 0;
    }
  }
}

static lonejson_read_result
cai_spooled_append_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_spooled_append_reader *reader;
  lonejson_read_result result;
  size_t count;

  result = lonejson_default_read_result();
  reader = (cai_spooled_append_reader *)user;
  count = 0U;
  if (reader == NULL || buffer == NULL || capacity == 0U) {
    result.eof = 1;
    return result;
  }
  while (count < capacity) {
    if (!cai_spooled_append_next_byte(reader, &buffer[count])) {
      break;
    }
    count++;
  }
  result.bytes_read = count;
  result.eof = reader->phase == 4 && reader->escape_pos >= reader->escape_len;
  if (reader->failed) {
    result.error_code = 1;
  }
  return result;
}

lonejson_status cai_lonejson_spooled_append(lonejson_spooled *value,
                                            const void *data, size_t len,
                                            lonejson_error *error) {
  static const unsigned char prefix[] = "{\"value\":\"";
  static const unsigned char suffix[] = "\"}";
  cai_spooled_append_reader reader;
  cai_spooled_append_doc doc;
  lonejson_spool_options options;
  lonejson_field fields[1];
  lonejson_map map;
  lonejson_spooled old_value;
  lonejson_error rewind_error;
  lonejson_status status;

  if (value == NULL || (data == NULL && len > 0U)) {
    if (error != NULL) {
      lonejson_error_init(error);
      error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "spool and data are required");
    }
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  if (len == 0U) {
    return LONEJSON_STATUS_OK;
  }

  memset(&reader, 0, sizeof(reader));
  memset(&doc, 0, sizeof(doc));
  old_value = *value;
  options = lonejson_default_spool_options();
  options.memory_limit = old_value.memory_limit;
  options.max_bytes = old_value.max_bytes;
  options.temp_dir = old_value.temp_dir;
  fields[0] = cai_spooled_append_fields[0];
  fields[0].spool_options = &options;
  map.name = "cai_spooled_append_doc";
  map.struct_size = sizeof(cai_spooled_append_doc);
  map.fields = fields;
  map.field_count = 1U;
  reader.prefix = prefix;
  reader.prefix_len = sizeof(prefix) - 1U;
  reader.suffix = suffix;
  reader.suffix_len = sizeof(suffix) - 1U;
  reader.data = (const unsigned char *)data;
  reader.data_len = len;
  reader.has_old = lonejson_spooled_size(&old_value) > 0U;
  if (reader.has_old) {
    reader.old_cursor = old_value;
    lonejson_error_init(&rewind_error);
    status = lonejson_spooled_rewind(&reader.old_cursor, &rewind_error);
    if (status != LONEJSON_STATUS_OK) {
      if (error != NULL) {
        *error = rewind_error;
      }
      return status;
    }
  }

  status = lonejson_parse_reader(&map, &doc, cai_spooled_append_read, &reader,
                                 NULL, error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&map, &doc);
    return status;
  }

  lonejson_spooled_cleanup(value);
  *value = doc.value;
  memset(&doc.value, 0, sizeof(doc.value));
  return LONEJSON_STATUS_OK;
}
