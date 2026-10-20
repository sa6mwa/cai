#ifndef CAI_CLI_MESSAGES_H
#define CAI_CLI_MESSAGES_H
#include "pouch.h"
#include <softline/softline.h>

typedef struct cai_cli_messages cai_cli_messages;
/* All UI methods are owner-thread-only. Service callbacks never touch softline.
 * The pouch/client/logger are borrowed until close, after the runtime closes.
 */
int cai_cli_messages_new(cai_cli_pouch *pouch, long ack_timeout_ms,
                         cai_cli_messages **out, cai_error *error);
int cai_cli_messages_register(void *context, cai_agent *agent,
                              cai_error *error);
int cai_cli_messages_start(cai_cli_messages *messages, const char *session_id,
                           const char *workspace, cai_error *error);
int cai_cli_messages_fd(const cai_cli_messages *messages);
int cai_cli_messages_sync(cai_cli_messages *messages, sl_t *sl,
                          cai_error *error);
int cai_cli_messages_consumed(cai_cli_messages *messages, sl_prompt_id_t id,
                              cai_error *error);
int cai_cli_messages_is_message(const cai_cli_messages *messages,
                                sl_prompt_id_t id);
void cai_cli_messages_rebind(cai_cli_messages *messages, sl_prompt_id_t old_id,
                             sl_prompt_id_t new_id);
void cai_cli_messages_detach(cai_cli_messages *messages, sl_t *sl);
void cai_cli_messages_stop(cai_cli_messages *messages);
void cai_cli_messages_cancel_send(cai_cli_messages *messages);
void cai_cli_messages_close(cai_cli_messages *messages);
#endif
