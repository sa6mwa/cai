#define CAI_LUA_CONFIG "cai.config"
typedef struct cai_lua_config_handle {
  cai_config *ptr;
  int busy;
} cai_lua_config_handle;
static const char *cai_lua_config_string(lua_State *L, int index) {
  size_t size;
  const char *value = luaL_checklstring(L, index, &size);
  if (memchr(value, '\0', size) != NULL)
    luaL_argerror(L, index,
                  "embedded NUL is not supported in configuration strings");
  return value;
}
static cai_config *cai_lua_config_check(lua_State *L) {
  cai_lua_config_handle *handle = luaL_checkudata(L, 1, CAI_LUA_CONFIG);
  if (handle->ptr == NULL)
    luaL_error(L, "configuration is closed");
  if (handle->busy)
    luaL_error(L, "configuration callback is active");
  return handle->ptr;
}
static int cai_lua_config_close(lua_State *L) {
  cai_lua_config_handle *handle = luaL_checkudata(L, 1, CAI_LUA_CONFIG);
  if (handle->busy)
    return luaL_error(L, "configuration callback is active");
  cai_config_close(handle->ptr);
  handle->ptr = NULL;
  return 0;
}
static int cai_lua_config_new(lua_State *L) {
  cai_config *ptr = NULL;
  cai_error error;
  cai_lua_config_handle *handle;
  int rc;
  handle = lua_newuserdata(L, sizeof(*handle));
  memset(handle, 0, sizeof(*handle));
  luaL_setmetatable(L, CAI_LUA_CONFIG);
  cai_error_init(&error);
  rc = cai_config_open(NULL, 0U, &ptr, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  handle->ptr = ptr;
  cai_lua_error_cleanup(&error);
  return 1;
}
static int cai_lua_config_load(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  cai_lua_config_handle *handle = luaL_checkudata(L, 1, CAI_LUA_CONFIG);
  const char *origin =
      lua_isnoneornil(L, 3) ? "configuration" : cai_lua_config_string(L, 3);
  cai_lua_source_ctx context;
  cai_source *source = NULL;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_lua_make_source(L, 2, &context, &source, &error);
  if (rc == CAI_OK) {
    handle->busy = 1;
    rc = self->load(self, source, origin, &error);
    handle->busy = 0;
  }
  cai_source_close(source);
  if (context.callback_ref != 0)
    luaL_unref(L, LUA_REGISTRYINDEX, context.callback_ref);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}
static int cai_lua_config_template(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  cai_lua_config_handle *handle = luaL_checkudata(L, 1, CAI_LUA_CONFIG);
  cai_lua_sink_ctx context;
  cai_sink *sink = NULL;
  cai_error error;
  int rc, top = lua_gettop(L);
  memset(&context, 0, sizeof(context));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 2, &context, &sink, &error);
  if (rc == CAI_OK) {
    handle->busy = 1;
    rc = self->write_template(self, sink, &error);
    handle->busy = 0;
  }
  cai_sink_close(sink);
  if (context.callback_ref != 0)
    luaL_unref(L, LUA_REGISTRYINDEX, context.callback_ref);
  lua_settop(L, top);
  if (rc != CAI_OK) {
    if (error.message == NULL)
      cai_lua_set_error(
          &error, rc, "configuration template output failed or was cancelled");
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}
static int cai_lua_config_path(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *section = cai_lua_config_string(L, 2),
             *key = cai_lua_config_string(L, 3);
  const char *path = NULL;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = self->resolve_path(self, section, key, &path, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  lua_pushstring(L, path);
  return 1;
}
static int cai_lua_config_load_file(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  cai_error error;
  int rc;
  const char *path = cai_lua_config_string(L, 2);
  cai_error_init(&error);
  rc = self->load_file(self, path, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}
static int cai_lua_config_get(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *value =
      self->get(self, cai_lua_config_string(L, 2), cai_lua_config_string(L, 3));
  lua_pushstring(L, value);
  return 1;
}
static int cai_lua_config_bool(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *section = cai_lua_config_string(L, 2),
             *key = cai_lua_config_string(L, 3);
  lua_pushboolean(L, self->get_bool(self, section, key, lua_toboolean(L, 4)));
  return 1;
}
static int cai_lua_config_integer(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *section = cai_lua_config_string(L, 2),
             *key = cai_lua_config_string(L, 3);
  lua_pushinteger(
      L, (lua_Integer)self->get_integer(self, section, key,
                                        (long long)luaL_optinteger(L, 4, 0)));
  return 1;
}
static int cai_lua_config_real(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *section = cai_lua_config_string(L, 2),
             *key = cai_lua_config_string(L, 3);
  lua_pushnumber(L,
                 self->get_real(self, section, key, luaL_optnumber(L, 4, 0)));
  return 1;
}
static int cai_lua_config_list(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *section = cai_lua_config_string(L, 2),
             *key = cai_lua_config_string(L, 3);
  const char *const *values;
  size_t count, i;
  values = self->get_list(self, section, key, &count);
  lua_createtable(L, (int)count, 0);
  for (i = 0U; i < count; i++) {
    lua_pushstring(L, values[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}
static int cai_lua_config_set(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *section = cai_lua_config_string(L, 2),
             *key = cai_lua_config_string(L, 3), *value;
  cai_error error;
  int rc;
  value = lua_isboolean(L, 4) ? (lua_toboolean(L, 4) ? "true" : "false")
                              : cai_lua_config_string(L, 4);
  cai_error_init(&error);
  rc = self->set(self, section, key, value, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}
static int cai_lua_config_environment(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = self->apply_environment(self, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}
static int cai_lua_config_template_file(lua_State *L) {
  cai_config *self = cai_lua_config_check(L);
  const char *path = cai_lua_config_string(L, 2);
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = self->create_template_file(self, path, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}
static const luaL_Reg cai_lua_config_methods[] = {
    {"load", cai_lua_config_load},
    {"load_file", cai_lua_config_load_file},
    {"write_template", cai_lua_config_template},
    {"resolve_path", cai_lua_config_path},
    {"get", cai_lua_config_get},
    {"get_bool", cai_lua_config_bool},
    {"get_integer", cai_lua_config_integer},
    {"get_real", cai_lua_config_real},
    {"get_list", cai_lua_config_list},
    {"set", cai_lua_config_set},
    {"apply_environment", cai_lua_config_environment},
    {"create_template_file", cai_lua_config_template_file},
    {"close", cai_lua_config_close},
    {NULL, NULL}};
