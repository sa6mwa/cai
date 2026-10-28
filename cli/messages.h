#ifndef CAI_CLI_MESSAGES_H
#define CAI_CLI_MESSAGES_H
#include "pouch.h"
#include <softline/softline.h>

typedef struct cai_cli_messages cai_cli_messages;
/* Recognized send receipts become Markdown for the interactive feed only.
 * Return 1 when formatted, 0 for unknown/invalid results or insufficient space.
 * The original tool result remains owned by the runtime. */
int cai_cli_message_delivery_markdown(const char *data, size_t length,
                                      char *out, size_t capacity);
/* Enqueue once without a delivery deadline, receipt listener or session. */
int cai_cli_messages_send_once(cai_cli_pouch *pouch, const char *recipient,
                               const char *text, cai_sink *result,
                               cai_error *error);
/* All UI methods are owner-thread-only. Service callbacks never touch softline.
 * The pouch/client/logger are borrowed until close, after the runtime closes.
 */
int cai_cli_messages_new(cai_cli_pouch *pouch, long ack_timeout_ms,
                         cai_cli_messages **out, cai_error *error);
int cai_cli_messages_register(void *context, cai_agent *agent,
                              cai_error *error);
int cai_cli_messages_start(cai_cli_messages *messages, const char *session_id,
                           const char *workspace, const char *queue_name,
                           cai_error *error);
const char *cai_cli_messages_name(const cai_cli_messages *messages);
/* Owner-thread-only, between runtime turns; keeps delivered inbox entries. */
int cai_cli_messages_set_queue(cai_cli_messages *messages, const char *name,
                               cai_error *error);
int cai_cli_messages_fd(const cai_cli_messages *messages);
int cai_cli_messages_sync(cai_cli_messages *messages, sl_t *sl,
                          cai_error *error);
int cai_cli_messages_consumed(cai_cli_messages *messages, sl_prompt_id_t id,
                              cai_error *error);
int cai_cli_messages_is_message(const cai_cli_messages *messages,
                                sl_prompt_id_t id);
void cai_cli_messages_detach(cai_cli_messages *messages, sl_t *sl);
void cai_cli_messages_stop(cai_cli_messages *messages);
/* Cancel sends already in progress, including lease acquisition and enqueue.
 * A later send starts with a fresh cancellation sequence. Thread-safe. */
void cai_cli_messages_cancel_send(cai_cli_messages *messages);
void cai_cli_messages_close(cai_cli_messages *messages);
/* Remove delivered inbox records owned by this closed conversation; preserve
 * shared workspace queues and records owned by other conversations. */
int cai_cli_messages_delete_session(cai_cli_pouch *pouch, const char *id,
                                    cai_error *error);
#endif
