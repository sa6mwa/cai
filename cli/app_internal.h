#ifndef CAI_APP_INTERNAL_H
#define CAI_APP_INTERNAL_H
#include <cai/app.h>
#include <pthread.h>
typedef struct cai_app_command {
  struct cai_app_command *next;
  cai_app_control_type type;
  char *text;
  unsigned long long id;
} cai_app_command;
typedef struct cai_app_bridge {
  pthread_mutex_t lock;
  int fd;
  int accepting;
  int failed;
  size_t count;
  unsigned long long next_id;
  cai_app_command *head, *tail;
  cai_app_status status;
  const cai_app_config *config;
} cai_app_bridge;
int cai_app_bridge_open(cai_app_bridge **out, const cai_app_config *config,
                        cai_error *error);
void cai_app_bridge_close(cai_app_bridge *bridge);
void cai_app_bridge_begin(cai_app_bridge *bridge);
void cai_app_bridge_end(cai_app_bridge *bridge, int code,
                        const cai_error *error);
int cai_app_bridge_control(cai_app_bridge *bridge, cai_app_control_type type,
                           const char *text, unsigned long long *id,
                           cai_error *error);
int cai_app_bridge_status(cai_app_bridge *bridge, cai_app_status *out,
                          cai_error *error);
int cai_app_bridge_publish(cai_app_bridge *bridge, cai_agent_runtime *runtime,
                           cai_error *error);
void cai_app_bridge_error(cai_app_bridge *bridge, const cai_error *error);
void cai_app_bridge_fail(cai_app_bridge *bridge, const cai_error *error);
cai_app_command *cai_app_bridge_take(cai_app_bridge *bridge);
/* Handoff owns wakeups temporarily. Leave commands queued for normal dispatch
 * after terminal restoration; drain notifications to avoid spinning while
 * non-STOP commands are deferred. A later submission signals again. */
int cai_app_bridge_shell_stop(cai_app_bridge *bridge);
void cai_app_bridge_ack(cai_app_bridge *bridge, cai_app_command *command,
                        int status, const cai_error *error);
void cai_app_bridge_outcome(cai_app_bridge *bridge, cai_app_outcome outcome);
int cai_cli_app_run(int argc, char **argv, const cai_app_config *application,
                    cai_app_bridge *bridge);
int cai_cli_app_main(int argc, char **argv, const cai_app_config *application);
size_t cai_cli_help_count(void);
const cai_app_option *cai_cli_help_at(size_t index);
int cai_cli_help_write(const char *program, cai_sink *sink, cai_error *error);
int cai_cli_help_print(const char *program);
int cai_cli_app_schema(cai_config **out, cai_error *error);
#endif
