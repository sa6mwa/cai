#define _POSIX_C_SOURCE 200809L
/* Lua 5.5 facade over the public cai_app receiver. run() owns the calling
 * thread; observers reenter Lua only there, never from native tool workers.
 * Application strings/settings are copied by open; native logger interop is
 * retained until close. No application-private headers are required. */
#include "todo_lua.h"
#include <cai/app.h>
#include <dlfcn.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <pslog_lua.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define APP_MT "cai.app"
/* Facade bindings reference only the installed public configuration struct. */
typedef struct app_text_field {
  const char *name;
  size_t offset;
} app_text_field;
#define APP_TEXT_FIELD(n) {#n, offsetof(cai_app_config, n)}
static const app_text_field text_fields[] = {
    APP_TEXT_FIELD(program_name),
    APP_TEXT_FIELD(invocation_name),
    APP_TEXT_FIELD(environment_prefix),
    APP_TEXT_FIELD(namespace_prefix),
    APP_TEXT_FIELD(queue_prefix),
    APP_TEXT_FIELD(state_directory),
    APP_TEXT_FIELD(cache_directory),
    APP_TEXT_FIELD(data_directory),
    APP_TEXT_FIELD(history_directory),
    APP_TEXT_FIELD(clipboard_directory),
    APP_TEXT_FIELD(log_directory),
    APP_TEXT_FIELD(pouch_key_file),
    APP_TEXT_FIELD(banner),
    APP_TEXT_FIELD(config_file),
    APP_TEXT_FIELD(config_directory),
    APP_TEXT_FIELD(introduction_markdown)};
#undef APP_TEXT_FIELD
typedef int (*logger_ref_fn)(lua_State *, int, const pslog_field *, size_t,
                             pslog_lua_logger_ref *);
typedef void (*logger_unref_fn)(pslog_lua_logger_ref *);
typedef int (*logger_check_fn)(lua_State *, int, pslog_lua_logger_view *);
typedef struct app_lua {
  cai_app *app;
  void *logger_module;
  logger_unref_fn unref;
  pslog_lua_logger_ref logger;
  int has_logger;
  int running;
  cai_app_setting *settings;
  char **values;
  size_t setting_count;
  char *buffer;
  size_t used, capacity;
  cai_sink *sink;
  lua_State *state;
  int self_ref;
} app_lua;

int luaopen_cai_app(lua_State *L);
static int failure(lua_State *L, const char *message) {
  lua_pushnil(L);
  lua_pushstring(L, message);
  return 2;
}
static const char *plain_string(lua_State *L, int index) {
  size_t length;
  const char *value = lua_tolstring(L, index, &length);
  return value != NULL && strlen(value) == length ? value : NULL;
}
static void staging_close(app_lua *ud) {
  size_t i;
  for (i = 0U; i < ud->setting_count; i++) {
    free(ud->values[i]);
  }
  free(ud->settings);
  free(ud->values);
  ud->settings = NULL;
  ud->values = NULL;
  ud->setting_count = 0U;
}
static int close_app(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  if (ud != NULL) {
    if (ud->running)
      return failure(L, "cannot close a running application");
    cai_app_close(ud->app);
    ud->app = NULL;
    if (ud->has_logger)
      ud->unref(&ud->logger);
    ud->has_logger = 0;
    if (ud->logger_module != NULL)
      dlclose(ud->logger_module);
    ud->logger_module = NULL;
    staging_close(ud);
    cai_sink_close(ud->sink);
    ud->sink = NULL;
    free(ud->buffer);
    ud->buffer = NULL;
    ud->used = ud->capacity = 0U;
  }
  return 0;
}
static int retain_logger(lua_State *L, app_lua *ud, int index) {
  union {
    void *object;
    logger_ref_fn function;
  } ref;
  union {
    void *object;
    logger_unref_fn function;
  } unref;
  union {
    void *object;
    logger_check_fn function;
  } check;
  pslog_lua_logger_view view;
  const char *path;
  int top = lua_gettop(L);
  index = lua_absindex(L, index);
  lua_getglobal(L, "require");
  lua_pushliteral(L, "pslog.core");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    lua_settop(L, top);
    return -1;
  }
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "searchpath");
  lua_pushliteral(L, "pslog.core");
  lua_getfield(L, -3, "cpath");
  if (lua_pcall(L, 2, 1, 0) != LUA_OK || !lua_isstring(L, -1)) {
    lua_settop(L, top);
    return -1;
  }
  path = lua_tostring(L, -1);
  ud->logger_module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  lua_settop(L, top);
  if (ud->logger_module == NULL)
    return -1;
  ref.object = dlsym(ud->logger_module, "pslog_lua_ref_logger");
  unref.object = dlsym(ud->logger_module, "pslog_lua_unref_logger");
  check.object = dlsym(ud->logger_module, "pslog_lua_check_logger");
  if (ref.function == NULL || unref.function == NULL || check.function == NULL)
    return -1;
  memset(&view, 0, sizeof(view));
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  if (check.function(L, index, &view) != 0 ||
      (view.flags & PSLOG_LUA_LOGGER_NATIVE_SINK) == 0U)
    return -1;
  ud->unref = unref.function;
  ud->logger.size = sizeof(ud->logger);
  ud->logger.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  if (ref.function(L, index, NULL, 0U, &ud->logger) != 0)
    return -1;
  ud->has_logger = 1;
  return 0;
}
static int setting(lua_State *L, app_lua *ud, const char *section,
                   const char *key, int index) {
  cai_app_setting *grown;
  char **values;
  const char *value;
  char *owned;
  if (lua_type(L, index) == LUA_TBOOLEAN)
    value = lua_toboolean(L, index) ? "true" : "false";
  else if (lua_type(L, index) == LUA_TSTRING ||
           lua_type(L, index) == LUA_TNUMBER)
    value = plain_string(L, index);
  else
    return -1;
  if (value == NULL)
    return -1;
  owned = strdup(value);
  if (owned == NULL)
    return -1;
  grown = realloc(ud->settings, (ud->setting_count + 1U) * sizeof(*grown));
  if (grown == NULL) {
    free(owned);
    return -1;
  }
  ud->settings = grown;
  values = realloc(ud->values, (ud->setting_count + 1U) * sizeof(*values));
  if (values == NULL) {
    free(owned);
    return -1;
  }
  ud->values = values;
  values[ud->setting_count] = owned;
  grown[ud->setting_count].section = section;
  grown[ud->setting_count].key = key;
  grown[ud->setting_count++].value = owned;
  return 0;
}
static int read_settings(lua_State *L, app_lua *ud, int table) {
  int rc = 0;
  table = lua_absindex(L, table);
  lua_pushnil(L);
  while (rc == 0 && lua_next(L, table)) {
    const char *section =
        lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
    if (section == NULL || !lua_istable(L, -1))
      return -1;
    lua_pushnil(L);
    while (rc == 0 && lua_next(L, -2)) {
      const char *key =
          lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
      if (key == NULL)
        return -1;
      if (lua_istable(L, -1)) {
        size_t i, count = lua_rawlen(L, -1);
        for (i = 1U; i <= count && rc == 0; i++) {
          lua_rawgeti(L, -1, (lua_Integer)i);
          rc = setting(L, ud, section, key, -1);
          lua_pop(L, 1);
        }
      } else
        rc = setting(L, ud, section, key, -1);
      lua_pop(L, 1);
    }
    if (rc != 0)
      return rc;
    lua_pop(L, 1);
  }
  return rc;
}
static void text_field(lua_State *L, const char *key, const char *value) {
  if (value != NULL) {
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
  }
}
static void integer_field(lua_State *L, const char *key, lua_Integer value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);
}
typedef struct observation {
  app_lua *app;
  const cai_agent_runtime_event *event;
  const cai_agent_session_event *replay;
} observation;
/* Table construction is inside pcall too: Lua allocation errors must not
 * unwind through the runtime or bypass application process restoration. */
static int observe_call(lua_State *L) {
  observation *call = lua_touserdata(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, call->app->self_ref);
  lua_getiuservalue(L, -1, 1);
  lua_getfield(L, -1, call->event != NULL ? "on_event" : "on_replay");
  lua_newtable(L);
  if (call->event != NULL) {
    const cai_agent_runtime_event *e = call->event;
    integer_field(L, "type", e->type);
    integer_field(L, "state", e->state);
    integer_field(L, "sequence", (lua_Integer)e->sequence);
    if (e->data != NULL) {
      lua_pushlstring(L, e->data, e->data_length);
      lua_setfield(L, -2, "data");
    }
#define EVENT_TEXT(field) text_field(L, #field, e->field)
    EVENT_TEXT(tool_name);
    EVENT_TEXT(tool_path);
    EVENT_TEXT(tool_call_id);
    EVENT_TEXT(runtime_session_id);
    EVENT_TEXT(parent_tool_call_id);
    EVENT_TEXT(subagent_name);
    EVENT_TEXT(subagent_metadata_json);
    EVENT_TEXT(subagent_instruction);
    EVENT_TEXT(terminal_id);
#undef EVENT_TEXT
#define EVENT_INT(field) integer_field(L, #field, (lua_Integer)e->field)
    EVENT_INT(tool_action);
    EVENT_INT(tool_path_count);
    EVENT_INT(terminal_command_id);
    EVENT_INT(terminal_has_exit_code);
    EVENT_INT(terminal_exit_code);
    EVENT_INT(terminal_has_signal);
    EVENT_INT(terminal_signal);
    EVENT_INT(terminal_duration_ms);
    EVENT_INT(terminal_total_output_bytes);
    EVENT_INT(terminal_output_truncated);
    EVENT_INT(terminal_detached_processes_possible);
#undef EVENT_INT
  } else {
    const cai_agent_session_event *e = call->replay;
    text_field(L, "type", e->type);
    text_field(L, "data", e->data);
    integer_field(L, "sequence", (lua_Integer)e->sequence);
  }
  lua_call(L, 1, 0);
  return 0;
}
static int observe(observation *call, cai_error *error) {
  lua_State *L = call->app->state;
  int top = lua_gettop(L);
  int rc = CAI_OK;
  lua_pushcfunction(L, observe_call);
  lua_pushlightuserdata(L, call);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    const char *message = lua_tostring(L, -1);
    cai_error_cleanup(error);
    error->message = strdup(message != NULL ? message : "Lua observer failed");
    rc = error->code = error->message != NULL ? CAI_ERR_INVALID : CAI_ERR_NOMEM;
  }
  lua_settop(L, top);
  return rc;
}
static int observe_event(void *context, const cai_agent_runtime_event *event,
                         cai_error *error) {
  observation call;
  call.app = context;
  call.event = event;
  call.replay = NULL;
  return observe(&call, error);
}
static int observe_replay(void *context, const cai_agent_session_event *event,
                          cai_error *error) {
  observation call;
  call.app = context;
  call.event = NULL;
  call.replay = event;
  return observe(&call, error);
}
static int open_app(lua_State *L) {
  static const char *const margins[] = {"introduction_margin_left",
                                        "introduction_margin_right"};
  cai_app_config config;
  cai_error error;
  app_lua *ud;
  int *margin_values[2];
  size_t i;
  int rc;
  if (!lua_isnoneornil(L, 1) && !lua_istable(L, 1))
    return failure(L, "application configuration must be a table");
  cai_app_config_init(&config);
  ud = lua_newuserdatauv(L, sizeof(*ud), 1);
  memset(ud, 0, sizeof(*ud));
  ud->state = L;
  ud->self_ref = LUA_NOREF;
  luaL_setmetatable(L, APP_MT);
  margin_values[0] = &config.introduction_margin_left;
  margin_values[1] = &config.introduction_margin_right;
  if (lua_istable(L, 1)) {
    lua_pushnil(L);
    while (lua_next(L, 1)) {
      const char *key =
          lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
      int known = 0;
      for (i = 0U;
           key != NULL && i < sizeof(text_fields) / sizeof(text_fields[0]); i++)
        if (strcmp(key, text_fields[i].name) == 0)
          known = 1;
      for (i = 0U; key != NULL && i < sizeof(margins) / sizeof(margins[0]); i++)
        if (strcmp(key, margins[i]) == 0)
          known = 1;
      if (key != NULL &&
          (strcmp(key, "settings") == 0 || strcmp(key, "no_config") == 0 ||
           strcmp(key, "logger") == 0 || strcmp(key, "on_event") == 0 ||
           strcmp(key, "on_replay") == 0))
        known = 1;
      if (!known)
        return failure(L, "unknown application configuration field");
      lua_pop(L, 1);
    }
    for (i = 0U; i < sizeof(text_fields) / sizeof(text_fields[0]); i++) {
      const char **value =
          (const char **)((char *)&config + text_fields[i].offset);
      lua_getfield(L, 1, text_fields[i].name);
      if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
          return failure(L, "application text fields must be strings");
        *value = plain_string(L, -1);
        if (*value == NULL)
          return failure(L, "application text cannot contain NUL bytes");
      }
      lua_pop(L, 1);
    }
    for (i = 0U; i < sizeof(margins) / sizeof(margins[0]); i++) {
      lua_getfield(L, 1, margins[i]);
      if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0 ||
            lua_tointeger(L, -1) > INT_MAX)
          return failure(L, "introduction margins must be nonnegative integers "
                            "within int range");
        *margin_values[i] = (int)lua_tointeger(L, -1);
      }
      lua_pop(L, 1);
    }
    lua_getfield(L, 1, "no_config");
    if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
      return failure(L, "no_config must be boolean");
    config.no_config = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "settings");
    if (!lua_isnil(L, -1) &&
        (!lua_istable(L, -1) || read_settings(L, ud, -1) != 0))
      return failure(L,
                     "settings must contain section tables with scalar values");
    lua_pop(L, 1);
    config.settings = ud->settings;
    config.setting_count = ud->setting_count;
    lua_getfield(L, 1, "logger");
    if (!lua_isnil(L, -1)) {
      if (retain_logger(L, ud, -1) != 0)
        return failure(L, "logger must be a live native-sink pslog logger with "
                          "public Lua interop");
      config.logger = ud->logger.borrowed;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "on_event");
    if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
      return failure(L, "on_event must be a function");
    if (lua_isfunction(L, -1)) {
      config.event_callback = observe_event;
      config.event_context = ud;
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "on_replay");
    if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
      return failure(L, "on_replay must be a function");
    if (lua_isfunction(L, -1)) {
      config.replay_callback = observe_replay;
      config.replay_context = ud;
    }
    lua_pop(L, 1);
    /* Keep functions on the userdata, not rooted in the registry: a callback
     * may capture its app without creating an uncollectable registry cycle. */
    lua_newtable(L);
    lua_getfield(L, 1, "on_event");
    lua_setfield(L, -2, "on_event");
    lua_getfield(L, 1, "on_replay");
    lua_setfield(L, -2, "on_replay");
    lua_setiuservalue(L, 2, 1);
  }
  cai_error_init(&error);
  rc = cai_app_open(&config, &ud->app, &error);
  staging_close(ud);
  if (rc != CAI_OK) {
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
    cai_error_cleanup(&error);
    return 2;
  }
  cai_error_cleanup(&error);
  return 1;
}
static int run_app(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  const char **argv;
  size_t i, count;
  cai_error error;
  int rc, code = 1;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  if (ud->running)
    return failure(L, "application is already running");
  if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2))
    return failure(L, "arguments must be an array without a program name");
  count = lua_istable(L, 2) ? lua_rawlen(L, 2) : 0U;
  if (count >= INT_MAX || count > (size_t)-1 / sizeof(*argv))
    return failure(L, "too many application arguments");
  argv = lua_newuserdatauv(L, (count + 1U) * sizeof(*argv), 0);
  for (i = 0U; i < count; i++) {
    lua_rawgeti(L, 2, (lua_Integer)i + 1);
    if (lua_type(L, -1) != LUA_TSTRING)
      return failure(L, "each application argument must be a string");
    argv[i] = plain_string(L, -1);
    if (argv[i] == NULL)
      return failure(L, "application arguments cannot contain NUL bytes");
    lua_pop(L, 1);
  }
  cai_error_init(&error);
  ud->state = L;
  lua_pushvalue(L, 1);
  ud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  ud->running = 1;
  rc = ud->app->run(ud->app, (int)count, argv, &code, &error);
  ud->running = 0;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->self_ref);
  ud->self_ref = LUA_NOREF;
  if (rc == CAI_OK)
    lua_pushinteger(L, code);
  else
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 1 : 2;
}
/* app:options() is a 1-based copy of public help metadata. Missing short flags
 * and argument labels are nil; it does not parse argv or initialize a run. */
static int options(lua_State *L) {
  static const char *const types[] = {"flag", "string", "integer",
                                      "optional_string", "counter"};
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  size_t i;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  lua_newtable(L);
  for (i = 0U; i < ud->app->option_count(ud->app); i++) {
    const cai_app_option *o = ud->app->option_at(ud->app, i);
    lua_newtable(L);
    lua_pushstring(L, o->long_flag);
    lua_setfield(L, -2, "long_flag");
    if (o->short_flag != 0) {
      lua_pushlstring(L, &o->short_flag, 1U);
      lua_setfield(L, -2, "short_flag");
    }
    lua_pushstring(L, types[o->type]);
    lua_setfield(L, -2, "type");
    lua_pushstring(L, o->argument);
    lua_setfield(L, -2, "argument");
    lua_pushstring(L, o->description);
    lua_setfield(L, -2, "description");
    lua_pushboolean(L, o->repeatable);
    lua_setfield(L, -2, "repeatable");
    lua_pushboolean(L, o->negatable);
    lua_setfield(L, -2, "negatable");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}
static void push_error(lua_State *L, const cai_error *error) {
  lua_newtable(L);
  integer_field(L, "code", error->code);
  integer_field(L, "http_status", (lua_Integer)error->http_status);
  text_field(L, "message", error->message);
  text_field(L, "detail", error->detail);
  text_field(L, "server_code", error->server_code);
  text_field(L, "request_id", error->request_id);
}
static void push_settings(lua_State *L, const cai_agent_runtime_settings *s) {
  lua_newtable(L);
  integer_field(L, "present", s->present);
  text_field(L, "model", s->model);
  text_field(L, "reasoning_effort", s->reasoning_effort);
  text_field(L, "reasoning_summary", s->reasoning_summary);
  text_field(L, "service_tier", s->service_tier);
}
static void boolean_field(lua_State *L, const char *key, int value) {
  lua_pushboolean(L, value);
  lua_setfield(L, -2, key);
}
/* Copy owned C snapshots to Lua values. session_usage token counters are
 * flattened beside spend estimates; there is no nested C-style usage member.
 * Runtime state is numeric; application outcome is a string. */
static int push_status(lua_State *L) {
  static const char *const outcomes[] = {"not_run", "running",   "completed",
                                         "stopped", "cancelled", "failed"};
  const cai_app_status *s = lua_touserdata(L, 1);
  const cai_agent_runtime_metrics *m = &s->metrics;
  const cai_token_usage *usage = &m->session_usage.usage;
  lua_newtable(L);
  boolean_field(L, "running", s->running);
  boolean_field(L, "runtime_ready", s->runtime_ready);
  text_field(L, "outcome", outcomes[s->outcome]);
  integer_field(L, "state", s->state);
  integer_field(L, "exit_code", s->exit_code);
  text_field(L, "session_id", s->session_id);
  push_settings(L, &s->effective);
  lua_setfield(L, -2, "effective");
  push_settings(L, &s->pending);
  lua_setfield(L, -2, "pending");
  lua_newtable(L);
  boolean_field(L, "has_goal", s->goal.has_goal);
  text_field(L, "objective", s->goal.objective);
  text_field(L, "status", s->goal.status);
  boolean_field(L, "has_token_budget", s->goal.has_token_budget);
#define GOAL_INT(field) integer_field(L, #field, (lua_Integer)s->goal.field)
  GOAL_INT(token_budget);
  GOAL_INT(tokens_used);
  GOAL_INT(remaining_tokens);
  GOAL_INT(elapsed_seconds);
  GOAL_INT(created_at);
  GOAL_INT(updated_at);
#undef GOAL_INT
  lua_setfield(L, -2, "goal");
  lua_newtable(L);
#define METRIC_INT(field) integer_field(L, #field, (lua_Integer)m->field)
  METRIC_INT(context_window_tokens);
  METRIC_INT(context_used_tokens);
  METRIC_INT(turn_elapsed_ms);
  METRIC_INT(last_turn_duration_ms);
  METRIC_INT(last_turn_finished_unix_seconds);
#undef METRIC_INT
  boolean_field(L, "has_context_usage", m->has_context_usage);
  boolean_field(L, "turn_active", m->turn_active);
  boolean_field(L, "has_last_turn", m->has_last_turn);
  lua_newtable(L);
#define USAGE_INT(field) integer_field(L, #field, (lua_Integer)usage->field)
  USAGE_INT(input_tokens);
  USAGE_INT(input_cached_tokens);
  USAGE_INT(output_tokens);
  USAGE_INT(output_reasoning_tokens);
  USAGE_INT(total_tokens);
#undef USAGE_INT
  lua_pushnumber(L, m->session_usage.estimated_spend_usd);
  lua_setfield(L, -2, "estimated_spend_usd");
  lua_pushnumber(L, m->session_usage.estimated_spend_eur);
  lua_setfield(L, -2, "estimated_spend_eur");
  boolean_field(L, "limit_exceeded", m->session_usage.limit_exceeded);
  lua_setfield(L, -2, "session_usage");
  lua_setfield(L, -2, "metrics");
  push_error(L, &s->error);
  lua_setfield(L, -2, "error");
  integer_field(L, "control_id", (lua_Integer)s->control_id);
  integer_field(L, "control_status", s->control_status);
  push_error(L, &s->control_error);
  lua_setfield(L, -2, "control_error");
  return 1;
}
static int status(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  cai_app_status snapshot;
  cai_error error;
  int rc;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  cai_app_status_init(&snapshot);
  cai_error_init(&error);
  rc = ud->app->get_status(ud->app, &snapshot, &error);
  if (rc != CAI_OK) {
    failure(L, error.message);
    cai_error_cleanup(&error);
    return 2;
  }
  lua_pushcfunction(L, push_status);
  lua_pushlightuserdata(L, &snapshot);
  rc = lua_pcall(L, 1, 1, 0);
  cai_app_status_cleanup(&snapshot);
  cai_error_cleanup(&error);
  return rc == LUA_OK ? 1 : lua_error(L);
}
static int control(lua_State *L) {
  static const char *const names[] = {
      "queue_prompt", "steer",      "cancel_turn", "stop",
      "replace_goal", "pause_goal", "resume_goal", "clear_goal"};
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  const char *name = lua_type(L, 2) == LUA_TSTRING ? plain_string(L, 2) : NULL;
  const char *text = NULL;
  unsigned long long id;
  cai_error error;
  size_t i;
  int rc;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  for (i = 0U; name != NULL && i < sizeof(names) / sizeof(names[0]); i++)
    if (strcmp(name, names[i]) == 0)
      break;
  if (name == NULL || i == sizeof(names) / sizeof(names[0]))
    return failure(L, "unknown host control");
  if (!lua_isnoneornil(L, 3)) {
    text = lua_type(L, 3) == LUA_TSTRING ? plain_string(L, 3) : NULL;
    if (text == NULL)
      return failure(L, "control text must be a string without NUL bytes");
  }
  cai_error_init(&error);
  rc = ud->app->control(ud->app, (cai_app_control_type)(i + 1U), text, &id,
                        &error);
  if (rc == CAI_OK)
    lua_pushinteger(L, (lua_Integer)id);
  else
    failure(L, error.message);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 1 : 2;
}
static int help_write(void *context, const void *bytes, size_t count,
                      cai_error *error) {
  app_lua *ud = context;
  char *grown;
  size_t capacity;
  (void)error;
  if (count > (size_t)-1 - ud->used)
    return CAI_ERR_LIMIT;
  if (ud->used + count > ud->capacity) {
    capacity = ud->used + count;
    grown = realloc(ud->buffer, capacity);
    if (grown == NULL)
      return CAI_ERR_NOMEM;
    ud->buffer = grown;
    ud->capacity = capacity;
  }
  memcpy(ud->buffer + ud->used, bytes, count);
  ud->used += count;
  return CAI_OK;
}
static int help(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  cai_sink_callbacks callbacks;
  cai_error error;
  int rc;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = ud;
  callbacks.write = help_write;
  ud->used = 0U;
  cai_error_init(&error);
  rc = cai_sink_from_callbacks(&callbacks, &ud->sink, &error);
  if (rc == CAI_OK)
    rc = ud->app->write_help(ud->app, ud->sink, &error);
  cai_sink_close(ud->sink);
  ud->sink = NULL;
  if (rc == CAI_OK)
    lua_pushlstring(L, ud->buffer, ud->used);
  else
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 1 : 2;
}
static int open_kanban(lua_State *L) {
  static const char *const names[] = {"state_directory",  "lockd",
                                      "lockd_client_pem", "default_board",
                                      "program_name",     "namespace_prefix",
                                      "todo_namespace",   "pouch_key_file"};
  cai_app_kanban_config config;
  const char **fields[sizeof(names) / sizeof(names[0])];
  cai_todo *todo = NULL;
  cai_error error;
  app_lua *owner;
  size_t i;
  int rc, owner_index;
  if (!lua_isnoneornil(L, 1) && !lua_istable(L, 1))
    return failure(L, "kanban configuration must be a table");
  cai_app_kanban_config_init(&config);
  owner = lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  luaL_setmetatable(L, APP_MT);
  owner_index = lua_gettop(L);
  fields[0] = &config.state_directory;
  fields[1] = &config.lockd;
  fields[2] = &config.lockd_client_pem;
  fields[3] = &config.default_board;
  fields[4] = &config.program_name;
  fields[5] = &config.namespace_prefix;
  fields[6] = &config.todo_namespace;
  fields[7] = &config.pouch_key_file;
  if (lua_istable(L, 1)) {
    lua_pushnil(L);
    while (lua_next(L, 1)) {
      const char *key =
          lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
      int known = key != NULL && (strcmp(key, "logger") == 0 ||
                                  strcmp(key, "max_result_items") == 0);
      for (i = 0U; key != NULL && i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(key, names[i]) == 0)
          known = 1;
      if (!known)
        return failure(L, "unknown kanban configuration field");
      lua_pop(L, 1);
    }
    for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
      lua_getfield(L, 1, names[i]);
      if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING || plain_string(L, -1) == NULL)
          return failure(L, "kanban text fields must be strings without NUL");
        *fields[i] = plain_string(L, -1);
      }
      lua_pop(L, 1);
    }
    lua_getfield(L, 1, "max_result_items");
    if (!lua_isnil(L, -1)) {
      if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0 ||
          (lua_Unsigned)lua_tointeger(L, -1) > (size_t)-1)
        return failure(L, "max_result_items must be a nonnegative size");
      config.max_result_items = (size_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "logger");
    if (!lua_isnil(L, -1)) {
      if (retain_logger(L, owner, -1) != 0)
        return failure(L, "kanban logger must have a native sink");
      config.logger = owner->logger.borrowed;
    }
    lua_pop(L, 1);
  }
  cai_error_init(&error);
  rc = cai_app_kanban_open(&config, &todo, &error);
  if (rc != CAI_OK) {
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
    cai_error_cleanup(&error);
    return 2;
  }
  cai_error_cleanup(&error);
  return cai_lua_todo_push(L, todo, owner_index);
}

int luaopen_cai_app(lua_State *L) {
  static const luaL_Reg methods[] = {
      {"run", run_app},     {"options", options},   {"help", help},
      {"control", control}, {"get_status", status}, {"close", close_app},
      {"__gc", close_app},  {"__close", close_app}, {NULL, NULL}};
  luaL_newmetatable(L, APP_MT);
  luaL_setfuncs(L, methods, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, open_app);
  lua_setfield(L, -2, "open");
  lua_pushcfunction(L, open_kanban);
  lua_setfield(L, -2, "kanban");
  return 1;
}
