#include <cai/agent_runtime.h>
#include <cai/cai.h>
#include <cai/mcp.h>
#include <cai/session_store.h>
#include <cai/tools/todo.h>

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

typedef struct native_todo_store_state {
  int begin_count;
  int destroy_count;
} native_todo_store_state;

typedef struct native_session_store_state {
  int checkpoint_count;
  int mcp_create_count;
  int mcp_load_count;
  int mcp_save_count;
  int mcp_destroy_count;
  int cleanup_count;
} native_session_store_state;

static native_todo_store_state native_todo_store;
static native_session_store_state native_session_store;
static int native_subagent_prepare_calls;

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

static int native_agent_session_checkpoint(
    void *context, const char *scope, const char *session_id, cai_source *state,
    unsigned long long applied_event_sequence, cai_error *error) {
  native_session_store_state *store;
  char buffer[256];

  store = (native_session_store_state *)context;
  while (state->read(state, buffer, sizeof(buffer), error) != 0U) {
  }
  if (error->code != CAI_OK) {
    return error->code;
  }
  store->checkpoint_count++;
  (void)scope;
  (void)session_id;
  (void)state;
  (void)applied_event_sequence;
  (void)error;
  return CAI_OK;
}

static int native_agent_session_load_latest(
    void *context, const char *scope, char *session_id,
    size_t session_id_capacity, cai_source **out,
    unsigned long long *out_applied_event_sequence, cai_error *error) {
  (void)context;
  (void)scope;
  (void)error;
  if (session_id_capacity > 0U) {
    session_id[0] = '\0';
  }
  *out = NULL;
  *out_applied_event_sequence = 0U;
  return CAI_OK;
}

static int native_agent_session_append_event(
    void *context, const char *scope, const char *session_id,
    const cai_agent_session_event *event, cai_error *error) {
  (void)context;
  (void)scope;
  (void)session_id;
  (void)event;
  (void)error;
  return CAI_OK;
}

static int native_agent_session_load_events_after(
    void *context, const char *scope, const char *session_id,
    unsigned long long after_sequence, cai_agent_session_event_fn callback,
    void *callback_context, cai_error *error) {
  (void)context;
  (void)scope;
  (void)session_id;
  (void)after_sequence;
  (void)callback;
  (void)callback_context;
  (void)error;
  return CAI_OK;
}

static cai_agent_session_store native_agent_session_store = {
    native_agent_session_checkpoint, native_agent_session_load_latest,
    native_agent_session_append_event, native_agent_session_load_events_after,
    &native_session_store};

static int native_mcp_session_create(void *context,
                                     const cai_mcp_session_state *initial,
                                     char *session_id,
                                     size_t session_id_capacity,
                                     cai_error *error) {
  static const char id[] = "native-session";

  (void)context;
  (void)initial;
  (void)error;
  native_session_store.mcp_create_count++;
  if (sizeof(id) > session_id_capacity) {
    return CAI_ERR_LIMIT;
  }
  memcpy(session_id, id, sizeof(id));
  return CAI_OK;
}

static int native_mcp_session_load(void *context, const char *session_id,
                                   cai_mcp_session_state *state,
                                   cai_error *error) {
  (void)context;
  (void)session_id;
  (void)error;
  memset(state, 0, sizeof(*state));
  state->initialized = 1;
  native_session_store.mcp_load_count++;
  return CAI_OK;
}

static int native_mcp_session_save(void *context, const char *session_id,
                                   const cai_mcp_session_state *state,
                                   cai_error *error) {
  (void)context;
  (void)session_id;
  (void)state;
  (void)error;
  native_session_store.mcp_save_count++;
  return CAI_OK;
}

static int native_mcp_session_destroy(void *context, const char *session_id,
                                      cai_error *error) {
  (void)context;
  (void)session_id;
  (void)error;
  native_session_store.mcp_destroy_count++;
  return CAI_OK;
}

static void native_mcp_session_cleanup(void *context) {
  native_session_store_state *store;

  store = (native_session_store_state *)context;
  store->cleanup_count++;
}

static cai_mcp_session_callbacks native_mcp_session_store = {
    native_mcp_session_create, native_mcp_session_load, native_mcp_session_save,
    native_mcp_session_destroy, native_mcp_session_cleanup};

static int native_subagent_prepare(
    void *context, const cai_agent_subagent_prepare_request *request,
    cai_agent_subagent_prepare_result *result, cai_error *error) {
  (void)context;
  (void)request;
  (void)error;
  native_subagent_prepare_calls++;
  result->child_input = "Native Lua backend prepared this task.";
  result->display_summary = "Preparing native delegated task.";
  result->metadata_json = "{\"source\":\"lua-native\"}";
  return CAI_OK;
}

static cai_agent_subagent_prepare_backend native_subagent_backend = {
    native_subagent_prepare, NULL};

static int native_store_new(lua_State *L, const char *kind, void *callbacks,
                            void *context, int has_context) {
  lua_getglobal(L, "require");
  if (!lua_isfunction(L, -1)) {
    return luaL_error(L, "Lua require function is unavailable");
  }
  lua_pushstring(L, "cai");
  lua_call(L, 1, 1);
  lua_getfield(L, -1, "native_store");
  if (!lua_isfunction(L, -1)) {
    return luaL_error(L, "cai.native_store is unavailable");
  }
  lua_pushstring(L, kind);
  lua_pushlightuserdata(L, callbacks);
  if (has_context) {
    lua_pushlightuserdata(L, context);
  } else {
    lua_pushnil(L);
  }
  lua_call(L, 3, 1);
  lua_remove(L, -2);
  return 1;
}

static int native_backend_new(lua_State *L, const char *kind, void *backend) {
  lua_getglobal(L, "require");
  if (!lua_isfunction(L, -1)) {
    return luaL_error(L, "Lua require function is unavailable");
  }
  lua_pushstring(L, "cai");
  lua_call(L, 1, 1);
  lua_getfield(L, -1, "native_backend");
  if (!lua_isfunction(L, -1)) {
    return luaL_error(L, "cai.native_backend is unavailable");
  }
  lua_pushstring(L, kind);
  lua_pushlightuserdata(L, backend);
  lua_call(L, 2, 1);
  lua_remove(L, -2);
  return 1;
}

static int native_todo_store_new(lua_State *L) {
  return native_store_new(L, "todo", &native_todo_store_callbacks,
                          &native_todo_store, 1);
}

static int native_agent_session_store_new(lua_State *L) {
  return native_store_new(L, "agent_session", &native_agent_session_store, NULL,
                          0);
}

static int native_mcp_session_store_new(lua_State *L) {
  return native_store_new(L, "mcp_session", &native_mcp_session_store,
                          &native_session_store, 1);
}

static int native_subagent_prepare_backend_new(lua_State *L) {
  return native_backend_new(L, "subagent_prepare", &native_subagent_backend);
}

static int native_todo_store_reset(lua_State *L) {
  (void)L;
  memset(&native_todo_store, 0, sizeof(native_todo_store));
  return 0;
}

static int native_session_store_reset(lua_State *L) {
  (void)L;
  memset(&native_session_store, 0, sizeof(native_session_store));
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

static int native_session_store_checkpoint_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_session_store.checkpoint_count);
  return 1;
}

static int native_session_store_cleanup_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_session_store.cleanup_count);
  return 1;
}

static int native_session_store_mcp_create_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_session_store.mcp_create_count);
  return 1;
}

static int native_session_store_mcp_load_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_session_store.mcp_load_count);
  return 1;
}

static int native_session_store_mcp_save_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_session_store.mcp_save_count);
  return 1;
}

static int native_session_store_mcp_destroy_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)native_session_store.mcp_destroy_count);
  return 1;
}

static int native_registry_make_free_reference(lua_State *L) {
  int reference;

  lua_pushboolean(L, 1);
  reference = luaL_ref(L, LUA_REGISTRYINDEX);
  luaL_unref(L, LUA_REGISTRYINDEX, reference);
  lua_pushinteger(L, (lua_Integer)reference);
  return 1;
}

int luaopen_cai_native_todo_store_test(lua_State *L) {
  static const luaL_Reg functions[] = {
      {"new", native_todo_store_new},
      {"reset", native_todo_store_reset},
      {"begin_count", native_todo_store_begin_count},
      {"destroy_count", native_todo_store_destroy_count},
      {"new_agent_session", native_agent_session_store_new},
      {"new_mcp_session", native_mcp_session_store_new},
      {"new_subagent_prepare_backend", native_subagent_prepare_backend_new},
      {"reset_sessions", native_session_store_reset},
      {"checkpoint_count", native_session_store_checkpoint_count},
      {"cleanup_count", native_session_store_cleanup_count},
      {"mcp_create_count", native_session_store_mcp_create_count},
      {"mcp_load_count", native_session_store_mcp_load_count},
      {"mcp_save_count", native_session_store_mcp_save_count},
      {"mcp_destroy_count", native_session_store_mcp_destroy_count},
      {"make_free_registry_reference", native_registry_make_free_reference},
      {NULL, NULL}};

  lua_newtable(L);
  luaL_setfuncs(L, functions, 0);
  return 1;
}
