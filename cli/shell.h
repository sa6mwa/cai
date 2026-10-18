#ifndef CAI_CLI_SHELL_H
#define CAI_CLI_SHELL_H

#include <cai/cai.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

/* Linux pipe execution for explicit user commands; independent of agent
 * tools/sandbox and never a PTY. Owner-thread callbacks consume output. */
typedef struct cai_cli_shell {
  pid_t pid;
  int output_fd;
  int exit_fd;
  int timer_fd;
  int exited;
  int eof;
  int exit_code;
  pthread_t waiter;
  int waiter_started;
  struct timespec started;
  unsigned char pending[4];
  size_t pending_length;
} cai_cli_shell;
typedef int (*cai_cli_shell_output_fn)(void *, const char *, size_t,
                                       cai_error *);

void cai_cli_shell_init(cai_cli_shell *shell);
int cai_cli_shell_start(cai_cli_shell *shell, const char *command,
                        const char *directory, cai_error *error);
/* Consume at most 64 KiB per dispatch; output precedes completion. */
int cai_cli_shell_pump(cai_cli_shell *shell, cai_cli_shell_output_fn output,
                       void *context, cai_error *error);
void cai_cli_shell_cancel(cai_cli_shell *shell);
unsigned long long cai_cli_shell_elapsed(const cai_cli_shell *shell);
void cai_cli_shell_close(cai_cli_shell *shell);
/* Literal terminal text: preserve UTF-8, LF and tabs, escape other controls
 * and invalid bytes. Retains at most one incomplete UTF-8 scalar. */
int cai_cli_shell_plain(cai_cli_shell *shell, const char *bytes, size_t length,
                        int final, cai_cli_shell_output_fn output,
                        void *context, cai_error *error);
/* Called with the editor released. Relay a real PTY in the foreground;
 * Optional output observes raw chunks before they reach the TTY. NULL relays
 * directly without recording terminal output.
 * Restores termios, signal dispositions and window handling on every exit.
 * status is the waitpid status (distinguishes exit 137 from SIGKILL). */
int cai_cli_shell_handoff(const char *path, const char *directory,
                          cai_cli_shell_output_fn output, void *context,
                          int *status, cai_error *error);

#endif
