#define _POSIX_C_SOURCE 200809L
#include "todo_lua.h"
#include <cai/app.h>
#include <dlfcn.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <pslog_lua.h>
#include <stdlib.h>
#include <string.h>

#define APP_MT "cai.app"
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
static int open_app(lua_State *L) {
  static const char *const names[] = {"program_name", "banner", "config_file",
                                      "config_directory"};
  cai_app_config config;
  cai_error error;
  app_lua *ud;
  const char **strings[4];
  size_t i;
  int rc;
  if (!lua_isnoneornil(L, 1) && !lua_istable(L, 1))
    return failure(L, "application configuration must be a table");
  cai_app_config_init(&config);
  ud = lua_newuserdatauv(L, sizeof(*ud), 0);
  memset(ud, 0, sizeof(*ud));
  luaL_setmetatable(L, APP_MT);
  strings[0] = &config.program_name;
  strings[1] = &config.banner;
  strings[2] = &config.config_file;
  strings[3] = &config.config_directory;
  if (lua_istable(L, 1)) {
    lua_pushnil(L);
    while (lua_next(L, 1)) {
      const char *key =
          lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
      int known = 0;
      for (i = 0U; key != NULL && i < 4U; i++)
        if (strcmp(key, names[i]) == 0)
          known = 1;
      if (key != NULL &&
          (strcmp(key, "settings") == 0 || strcmp(key, "no_config") == 0 ||
           strcmp(key, "logger") == 0))
        known = 1;
      if (!known)
        return failure(L, "unknown application configuration field");
      lua_pop(L, 1);
    }
    for (i = 0U; i < 4U; i++) {
      lua_getfield(L, 1, names[i]);
      if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
          return failure(L, "application text fields must be strings");
        *strings[i] = plain_string(L, -1);
        if (*strings[i] == NULL)
          return failure(L, "application text cannot contain NUL bytes");
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
  ud->running = 1;
  rc = ud->app->run(ud->app, (int)count, argv, &code, &error);
  ud->running = 0;
  if (rc == CAI_OK)
    lua_pushinteger(L, code);
  else
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 1 : 2;
}
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
  static const char *const names[] = {"state_directory", "lockd",
                                      "lockd_client_pem", "default_board"};
  cai_app_kanban_config config;
  const char **fields[4];
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
  if (lua_istable(L, 1)) {
    lua_pushnil(L);
    while (lua_next(L, 1)) {
      const char *key =
          lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
      int known = key != NULL && (strcmp(key, "logger") == 0 ||
                                  strcmp(key, "max_result_items") == 0);
      for (i = 0U; key != NULL && i < 4U; i++)
        if (strcmp(key, names[i]) == 0)
          known = 1;
      if (!known)
        return failure(L, "unknown kanban configuration field");
      lua_pop(L, 1);
    }
    for (i = 0U; i < 4U; i++) {
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
      {"run", run_app},     {"options", options}, {"help", help},
      {"close", close_app}, {"__gc", close_app},  {"__close", close_app},
      {NULL, NULL}};
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
