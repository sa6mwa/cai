#ifndef CAI_CLI_TODO_H
#define CAI_CLI_TODO_H
#include <cai/agent_runtime.h>
typedef int (*cai_cli_todo_write)(void *, const char *, size_t);
int cai_cli_todo_command(cai_agent_runtime *runtime, const char *arguments,
                         cai_cli_todo_write write, void *context,
                         cai_error *error);
int cai_cli_todo_status(cai_agent_runtime *runtime, cai_cli_todo_write write,
                        void *context, cai_error *error);
#endif
