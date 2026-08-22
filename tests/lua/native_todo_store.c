#include <cai/cai.h>
#include <cai/tools/todo.h>

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

typedef struct native_todo_store_state {
  int begin_count;
  int destroy_count;
} native_todo_store_state;

static native_todo_store_state native_todo_store;

int luaopen_cai_native_todo_store_test(lua_State *L);

static int native_todo_store_begin(void *context, void **transaction,
                                   cai_error *error) {
  native_todo_store_state *state;

  state = (native_todo_store_state *)context;
  state->begin_count++;
  *transaction = NULL;
  (void)error;
  return CAI_ERR_INVALID;
}

static int native_todo_store_open_read(void *context, void *transaction,
                                       lonejson_reader_fn *reader,
                                       void **reader_context,
                                       cai_error *error) {
  (void)context;
  (void)transaction;
  (void)reader;
  (void)reader_context;
  (void)error;
  return CAI_ERR_INVALID;
}

static void native_todo_store_close_read(void *context, void *transaction,
                                         void *reader_context) {
  (void)context;
  (void)transaction;
  (void)reader_context;
}

static int native_todo_store_open_write(void *context, void *transaction,
                                        lonejson_sink_fn *sink,
                                        void **sink_context, cai_error *error) {
  (void)context;
  (void)transaction;
  (void)sink;
  (void)sink_context;
  (void)error;
  return CAI_ERR_INVALID;
}

static int native_todo_store_commit_write(void *context, void *transaction,
                                          cai_error *error) {
  (void)context;
  (void)transaction;
  (void)error;
  return CAI_ERR_INVALID;
}

static int native_todo_store_commit(void *context, void *transaction,
                                    cai_error *error) {
  (void)context;
  (void)transaction;
  (void)error;
  return CAI_ERR_INVALID;
}

static void native_todo_store_rollback(void *context, void *transaction) {
  (void)context;
  (void)transaction;
}

static void native_todo_store_destroy(void *context) {
  native_todo_store_state *state;

  state = (native_todo_store_state *)context;
  state->destroy_count++;
}

static cai_todo_store_callbacks native_todo_store_callbacks = {
    native_todo_store_begin,        native_todo_store_open_read,
    native_todo_store_close_read,   native_todo_store_open_write,
    native_todo_store_commit_write, native_todo_store_commit,
    native_todo_store_rollback,     native_todo_store_destroy};

static int native_todo_store_new(lua_State *L) {
  lua_getglobal(L, "require");
  if (!lua_isfunction(L, -1)) {
    return luaL_error(L, "Lua require function is unavailable");
  }
  lua_pushstring(L, "cai");
  lua_call(L, 1, 1);
  lua_getfield(L, -1, "todo_store_from_native");
  if (!lua_isfunction(L, -1)) {
    return luaL_error(L, "cai.todo_store_from_native is unavailable");
  }
  lua_pushlightuserdata(L, (void *)&native_todo_store_callbacks);
  lua_pushlightuserdata(L, (void *)&native_todo_store);
  lua_call(L, 2, 1);
  lua_remove(L, -2);
  return 1;
}

static int native_todo_store_reset(lua_State *L) {
  (void)L;
  memset(&native_todo_store, 0, sizeof(native_todo_store));
  return 0;
}

static int native_todo_store_begin_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_todo_store.begin_count);
  return 1;
}

static int native_todo_store_destroy_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_todo_store.destroy_count);
  return 1;
}

int luaopen_cai_native_todo_store_test(lua_State *L) {
  static const luaL_Reg functions[] = {
      {"new", native_todo_store_new},
      {"reset", native_todo_store_reset},
      {"begin_count", native_todo_store_begin_count},
      {"destroy_count", native_todo_store_destroy_count},
      {NULL, NULL}};

  lua_newtable(L);
  luaL_setfuncs(L, functions, 0);
  return 1;
}
