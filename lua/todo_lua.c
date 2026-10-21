#include "todo_lua.h"
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>

#define TODO_MT "cai.todo"
typedef struct todo_lua {
  cai_todo *todo;
  int busy;
} todo_lua;
typedef struct todo_io {
  lua_State *L;
  int reader, writer, begin;
  const char *bytes;
  size_t count, offset;
  int chunk;
} todo_io;
static int io_error(cai_error *error, const char *text) {
  cai_error_cleanup(error);
  error->code = CAI_ERR_TRANSPORT;
  error->message = malloc(strlen(text) + 1U);
  if (error->message != NULL)
    strcpy(error->message, text);
  return CAI_ERR_TRANSPORT;
}
static int result(lua_State *L, int rc, cai_error *error) {
  if (rc == CAI_OK)
    lua_pushboolean(L, 1);
  else {
    lua_pushnil(L);
    lua_pushstring(L, error->message != NULL ? error->message
                                             : cai_status_string(rc));
  }
  cai_error_cleanup(error);
  return rc == CAI_OK ? 1 : 2;
}
static todo_lua *check(lua_State *L) { return luaL_checkudata(L, 1, TODO_MT); }
static int unavailable(lua_State *L) {
  lua_pushnil(L);
  lua_pushliteral(L, "kanban receiver is closed or busy");
  return 2;
}
static int close_todo(lua_State *L) {
  todo_lua *ud = check(L);
  if (ud->busy)
    return unavailable(L);
  cai_todo_close(ud->todo);
  ud->todo = NULL;
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 1);
  return 0;
}
static int write_todo(void *context, const void *bytes, size_t count,
                      cai_error *error) {
  todo_io *io = context;
  lua_pushvalue(io->L, io->writer);
  lua_pushlstring(io->L, bytes, count);
  if (lua_pcall(io->L, 1, 1, 0) != LUA_OK)
    return io_error(error, "kanban writer raised an error");
  if (lua_isboolean(io->L, -1) && !lua_toboolean(io->L, -1)) {
    lua_pop(io->L, 1);
    return io_error(error, "kanban writer rejected bytes");
  }
  lua_pop(io->L, 1);
  return CAI_OK;
}
static int begin_todo(void *context, int status, const char *type,
                      cai_error *error) {
  todo_io *io = context;
  if (io->begin == 0)
    return CAI_OK;
  lua_pushvalue(io->L, io->begin);
  lua_pushinteger(io->L, status);
  lua_pushstring(io->L, type);
  if (lua_pcall(io->L, 2, 1, 0) != LUA_OK)
    return io_error(error, "kanban header callback raised an error");
  if (lua_isboolean(io->L, -1) && !lua_toboolean(io->L, -1)) {
    lua_pop(io->L, 1);
    return io_error(error, "kanban header callback rejected response");
  }
  lua_pop(io->L, 1);
  return CAI_OK;
}
static size_t read_todo(void *context, void *bytes, size_t count,
                        cai_error *error) {
  todo_io *io = context;
  size_t take;
  if (io->reader != 0 && io->offset == io->count) {
    if (io->chunk != 0)
      luaL_unref(io->L, LUA_REGISTRYINDEX, io->chunk);
    io->chunk = 0;
    lua_pushvalue(io->L, io->reader);
    if (lua_pcall(io->L, 0, 1, 0) != LUA_OK) {
      io_error(error, "kanban reader raised an error");
      return 0U;
    }
    if (lua_isnil(io->L, -1)) {
      lua_pop(io->L, 1);
      io->reader = 0;
      return 0U;
    }
    if (lua_type(io->L, -1) != LUA_TSTRING) {
      lua_pop(io->L, 1);
      io_error(error, "kanban reader must return a string or nil");
      return 0U;
    }
    io->bytes = lua_tolstring(io->L, -1, &io->count);
    io->offset = 0U;
    if (io->count == 0U || io->count > 65536U) {
      lua_pop(io->L, 1);
      io_error(error, "kanban reader chunks must contain 1..65536 bytes");
      return 0U;
    }
    io->chunk = luaL_ref(io->L, LUA_REGISTRYINDEX);
  }
  take = io->count - io->offset;
  if (take > count)
    take = count;
  if (take != 0U)
    memcpy(bytes, io->bytes + io->offset, take);
  io->offset += take;
  return take;
}
static int call_todo(lua_State *L) {
  todo_lua *ud = check(L);
  todo_io io;
  cai_source_callbacks reader;
  cai_sink_callbacks writer;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_error error;
  int rc;
  if (ud->todo == NULL || ud->busy)
    return unavailable(L);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  memset(&io, 0, sizeof(io));
  io.L = L;
  io.writer = 3;
  if (lua_type(L, 2) == LUA_TFUNCTION)
    io.reader = 2;
  else
    io.bytes = luaL_checklstring(L, 2, &io.count);
  memset(&reader, 0, sizeof(reader));
  reader.context = &io;
  reader.read = read_todo;
  memset(&writer, 0, sizeof(writer));
  writer.context = &io;
  writer.write = write_todo;
  cai_error_init(&error);
  ud->busy = 1;
  rc = cai_source_from_callbacks(&reader, &source, &error);
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&writer, &sink, &error);
  if (rc == CAI_OK)
    rc = cai_todo_call(ud->todo, source, sink, &error);
  cai_source_close(source);
  cai_sink_close(sink);
  if (io.chunk != 0)
    luaL_unref(L, LUA_REGISTRYINDEX, io.chunk);
  ud->busy = 0;
  return result(L, rc, &error);
}
static int select_todo(lua_State *L) {
  todo_lua *ud = check(L);
  const char *name;
  size_t length;
  cai_error error;
  int rc;
  if (ud->todo == NULL || ud->busy)
    return unavailable(L);
  name = luaL_checklstring(L, 2, &length);
  if (strlen(name) != length)
    return luaL_error(L, "board contains NUL");
  cai_error_init(&error);
  rc = cai_todo_select(ud->todo, name, lua_toboolean(L, 3), &error);
  return result(L, rc, &error);
}
static int board_todo(lua_State *L) {
  todo_lua *ud = check(L);
  char name[CAI_TODO_BOARD_NAME_MAX + 1U];
  cai_error error;
  int rc;
  if (ud->todo == NULL || ud->busy)
    return unavailable(L);
  cai_error_init(&error);
  rc = cai_todo_get_board(ud->todo, name, sizeof(name), &error);
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  lua_pushstring(L, name);
  return 1;
}
static const char *field(lua_State *L, const char *name, const char *fallback) {
  const char *value;
  size_t length;
  lua_getfield(L, 2, name);
  value = lua_isnil(L, -1) ? fallback : luaL_checklstring(L, -1, &length);
  if (!lua_isnil(L, -1) && strlen(value) != length)
    luaL_error(L, "HTTP field contains NUL");
  /* Keep values pinned for the duration of the request. */
  return value;
}
static int http_todo(lua_State *L) {
  todo_lua *ud = check(L);
  cai_todo_http_request request;
  cai_todo_http_response response;
  cai_sink_callbacks writer;
  todo_io io;
  cai_error error;
  int rc;
  if (ud->todo == NULL || ud->busy)
    return unavailable(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&io, 0, sizeof(io));
  io.L = L;
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  request.method = field(L, "method", "GET");
  request.resource = field(L, "resource", "");
  request.board = field(L, "board", NULL);
  lua_getfield(L, 2, "write");
  luaL_checktype(L, -1, LUA_TFUNCTION);
  io.writer = lua_gettop(L);
  lua_getfield(L, 2, "begin");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TFUNCTION);
    io.begin = lua_gettop(L);
  }
  memset(&writer, 0, sizeof(writer));
  writer.write = write_todo;
  writer.context = &io;
  response.begin = begin_todo;
  response.context = &io;
  cai_error_init(&error);
  ud->busy = 1;
  rc = cai_sink_from_callbacks(&writer, &response.body, &error);
  if (rc == CAI_OK)
    rc = cai_todo_handle_http(ud->todo, &request, &response, &error);
  cai_sink_close(response.body);
  ud->busy = 0;
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  lua_newtable(L);
  lua_pushinteger(L, response.status);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, response.content_type);
  lua_setfield(L, -2, "content_type");
  return 1;
}
int cai_lua_todo_push(lua_State *L, cai_todo *todo, int owner) {
  static const luaL_Reg methods[] = {
      {"call", call_todo},       {"select", select_todo},
      {"get_board", board_todo}, {"handle_http", http_todo},
      {"close", close_todo},     {"__close", close_todo},
      {"__gc", close_todo},      {NULL, NULL}};
  todo_lua *ud;
  if (owner != 0)
    owner = lua_absindex(L, owner);
  luaL_newmetatable(L, TODO_MT);
  luaL_setfuncs(L, methods, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  ud = lua_newuserdatauv(L, sizeof(*ud), 1);
  ud->todo = todo;
  ud->busy = 0;
  luaL_setmetatable(L, TODO_MT);
  if (owner != 0) {
    lua_pushvalue(L, owner);
    lua_setiuservalue(L, -2, 1);
  }
  return 1;
}
