#ifndef CAI_TODO_LUA_H
#define CAI_TODO_LUA_H
#include <cai/tools/todo.h>
#include <lua.h>
/* Transfers a receiver reference; owner is pinned until close. */
int cai_lua_todo_push(lua_State *L, cai_todo *todo, int owner);
#endif
