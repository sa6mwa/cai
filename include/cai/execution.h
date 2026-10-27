/** @file cai/execution.h
 * Structured agent execution history, separate from model context and logs.
 */
#ifndef CAI_EXECUTION_H
#define CAI_EXECUTION_H
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Maximum decoded payload bytes in one execution record. Logical inputs and
 * outputs comprise consecutive chunks correlated by session/call/kind/field.
 * Offsets count original UTF-8 text bytes; NUL is not supported. final
 * terminates that field, including an empty field. Chunks never split a UTF-8
 * character. No full-value staging.
 */
#define CAI_EXECUTION_CHUNK_BYTES 4096U
/** Upper bound on one encoded record, excluding its outer journal envelope. */
#define CAI_EXECUTION_RECORD_MAX_BYTES 65536U

/** Owned decoded execution record. Initialize before parsing; cleanup after
 * use. The journal type is "execution"; runtime events use EXECUTION_RECORD.
 * This record is never input to inference, recovery actions or compaction.
 */
typedef struct cai_execution_record {
  const char *kind;
  const char *session_id;
  const char *parent_session_id;
  const char *parent_call_id;
  const char *subagent;
  const char *tool_name;
  const char *call_id;
  const char *field;
  const char *model;
  const char *base_url;
  /** Most recently committed provider response, when available. */
  const char *last_response_id;
  unsigned long long timestamp_ns;
  /** Journal sequence in session_id; zero when no source store is configured.
   * A copy forwarded to a parent has its own outer journal sequence. */
  unsigned long long source_sequence;
  unsigned long long offset;
  int final;
  int line_start;
  long long code;
  long long http_status;
  char data[CAI_EXECUTION_CHUNK_BYTES + 1U];
} cai_execution_record;

void cai_execution_record_init(cai_execution_record *record);
/** Parse one record (at most 64 KiB of JSON). Replaces an initialized record
 * on success and leaves it unchanged on failure. Record data is at most
 * CAI_EXECUTION_CHUNK_BYTES decoded UTF-8 text bytes; embedded NUL is rejected.
 */
int cai_execution_record_parse(const char *json, cai_execution_record *record,
                               cai_error *error);
void cai_execution_record_cleanup(cai_execution_record *record);
/** Render a decoded record as Markdown. compact shows lifecycle summaries and
 * failures; zero renders all payload chunks, including arguments and output.
 * Feed ordered records to the same sink to join chunks without added breaks.
 */
int cai_execution_record_write_markdown(const cai_execution_record *record,
                                        int compact, cai_sink *sink,
                                        cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
