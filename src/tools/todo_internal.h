#ifndef CAI_TODO_INTERNAL_H
#define CAI_TODO_INTERNAL_H
#include <cai/tools/todo.h>
int cai_todo_http_snapshot(cai_todo *self, const char *board, int head,
                           cai_todo_http_response *response, cai_error *error);
#endif
