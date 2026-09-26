#ifndef CAI_CLI_LOG_H
#define CAI_CLI_LOG_H

#include "pouch_stream.h"
#include <pslog.h>
#include <pthread.h>

typedef struct cai_cli_log {
  pslog_logger *root;
  pslog_logger *logger;
  pthread_mutex_t lock;
  int initialized;
  int interactive;
  int fd;
  int wakeup_fd;
  int failed;
  int bootstrap;
  char directory[4096];
  char path[4096];
  char prefix[4096];
  size_t prefix_length;
  char notices[32][1200];
  size_t head;
  size_t count;
  unsigned long dropped;
} cai_cli_log;

int cai_cli_log_open(cai_cli_log *log, pslog_level default_level,
                     int interactive, cai_error *error);
int cai_cli_log_interactive(cai_cli_log *log, cai_error *error);
int cai_cli_log_session(cai_cli_log *log, const char *id, cai_error *error);
int cai_cli_log_notices(cai_cli_log *log, int (*visit)(void *, const char *),
                        void *context);
int cai_cli_log_close(cai_cli_log *log);
void cai_cli_log_message(pslog_logger *logger, pslog_level level,
                         const char *event, const char *role, const char *bytes,
                         size_t count, const pslog_field *fields,
                         size_t field_count);
void cai_cli_log_error(pslog_logger *logger, const char *operation,
                       const cai_error *error);

#endif
