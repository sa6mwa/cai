#define CAI_LUA_SKILL_METADATA "cai.skill_metadata"
typedef struct cai_lua_skill_metadata {
  cai_skill_metadata *ptr;
} cai_lua_skill_metadata;
static cai_skill_metadata *cai_lua_skill_metadata_check(lua_State *L) {
  cai_lua_skill_metadata *handle =
      luaL_checkudata(L, 1, CAI_LUA_SKILL_METADATA);
  if (handle->ptr == NULL)
    luaL_error(L, "skill metadata is closed");
  return handle->ptr;
}
static int cai_lua_skill_metadata_close(lua_State *L) {
  cai_lua_skill_metadata *handle =
      luaL_checkudata(L, 1, CAI_LUA_SKILL_METADATA);
  cai_skill_metadata_close(handle->ptr);
  handle->ptr = NULL;
  return 0;
}
static int cai_lua_skill_metadata_name(lua_State *L) {
  cai_skill_metadata *self = cai_lua_skill_metadata_check(L);
  lua_pushstring(L, self->name(self));
  return 1;
}
static int cai_lua_skill_metadata_description(lua_State *L) {
  cai_skill_metadata *self = cai_lua_skill_metadata_check(L);
  lua_pushstring(L, self->description(self));
  return 1;
}
static int cai_lua_skill_metadata_new(lua_State *L) {
  cai_lua_skill_metadata *handle;
  cai_lua_source_ctx context;
  cai_source *source = NULL;
  cai_error error;
  size_t size;
  const char *fallback = luaL_checklstring(L, 2, &size);
  int rc;
  if (memchr(fallback, '\0', size) != NULL)
    return luaL_argerror(L, 2, "embedded NUL in fallback skill name");
  if (lua_type(L, 1) != LUA_TSTRING && lua_type(L, 1) != LUA_TFUNCTION)
    return luaL_argerror(L, 1, "expected skill document text or chunk reader");
  handle = lua_newuserdata(L, sizeof(*handle));
  handle->ptr = NULL;
  luaL_setmetatable(L, CAI_LUA_SKILL_METADATA);
  cai_error_init(&error);
  rc = cai_lua_make_source(L, 1, &context, &source, &error);
  if (rc == CAI_OK)
    rc = cai_skill_metadata_open(source, fallback, &handle->ptr, &error);
  cai_source_close(source);
  if (context.callback_ref != 0)
    luaL_unref(L, LUA_REGISTRYINDEX, context.callback_ref);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_error_cleanup(&error);
  return 1;
}
static const luaL_Reg cai_lua_skill_metadata_methods[] = {
    {"name", cai_lua_skill_metadata_name},
    {"description", cai_lua_skill_metadata_description},
    {"close", cai_lua_skill_metadata_close},
    {NULL, NULL}};
