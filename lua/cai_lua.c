#include <cai/cai.h>
#include <cai/mcp.h>
#include <cai/tools/revgeo.h>
#include <cai/tools/searxng.h>
#include <cai/tools/todo.h>

#include <lauxlib.h>
#include <lonejson.h>
#include <lua.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 502
#define lua_rawlen lua_objlen
static int cai_lua_absindex(lua_State *L, int index) {
  if (index > 0 || index <= LUA_REGISTRYINDEX) {
    return index;
  }
  return lua_gettop(L) + index + 1;
}
#define lua_absindex cai_lua_absindex
#endif

#if !defined(LUA_OK)
#define LUA_OK 0
#endif

#define CAI_LUA_CLIENT "cai.client"
#define CAI_LUA_AGENT "cai.agent"
#define CAI_LUA_SESSION "cai.session"
#define CAI_LUA_RESPONSE "cai.response"
#define CAI_LUA_OUTPUT "cai.output"
#define CAI_LUA_REGISTRY "cai.tool_registry"
#define CAI_LUA_MCP "cai.mcp_handler"
#define CAI_LUA_SCHEMA "cai.tool_schema"
#define CAI_LUA_PARAMS "cai.response_params"
#define CAI_LUA_CONVERSATION "cai.conversation"
#define CAI_LUA_INPUT_ITEMS "cai.input_item_list"
#define CAI_LUA_CONVERSATION_ITEM "cai.conversation_item"
#define CAI_LUA_CONVERSATION_PARAMS "cai.conversation_items_params"
#define CAI_LUA_SPOOL_READER "cai.spooled_reader"

int luaopen_cai(lua_State *L);

typedef struct cai_lua_tool_ref cai_lua_tool_ref;

struct cai_lua_tool_ref {
  int callback_ref;
  void *context;
  cai_lua_tool_ref *next;
};

typedef struct cai_lua_client {
  cai_client *ptr;
} cai_lua_client;

typedef struct cai_lua_agent {
  cai_agent *ptr;
  lua_State *L;
  cai_lua_tool_ref *tools;
} cai_lua_agent;

typedef struct cai_lua_session {
  cai_session *ptr;
} cai_lua_session;

typedef struct cai_lua_response {
  cai_response *ptr;
} cai_lua_response;

typedef struct cai_lua_output {
  cai_output *ptr;
} cai_lua_output;

typedef struct cai_lua_registry {
  cai_tool_registry *ptr;
  lua_State *L;
  cai_lua_tool_ref *tools;
} cai_lua_registry;

typedef struct cai_lua_mcp {
  cai_mcp_handler *ptr;
} cai_lua_mcp;

typedef struct cai_lua_schema {
  cai_tool_schema *ptr;
} cai_lua_schema;

typedef struct cai_lua_params {
  cai_response_create_params *ptr;
} cai_lua_params;

typedef struct cai_lua_conversation {
  cai_conversation *ptr;
} cai_lua_conversation;

typedef struct cai_lua_input_items {
  cai_input_item_list *ptr;
} cai_lua_input_items;

typedef struct cai_lua_conversation_item {
  cai_conversation_item *ptr;
} cai_lua_conversation_item;

typedef struct cai_lua_conversation_params {
  cai_conversation_items_params *ptr;
} cai_lua_conversation_params;

typedef struct cai_lua_spool_reader {
  lonejson_spooled cursor;
  int owns_cursor;
} cai_lua_spool_reader;

typedef struct cai_lua_sink_ctx {
  lua_State *L;
  int callback_ref;
} cai_lua_sink_ctx;

typedef struct cai_lua_source_ctx {
  lua_State *L;
  int callback_ref;
  const char *bytes;
  size_t len;
  size_t pos;
  char *chunk;
  size_t chunk_len;
  size_t chunk_pos;
  int done;
} cai_lua_source_ctx;

typedef struct cai_lua_headers_ctx {
  lua_State *L;
  int get_ref;
  int set_ref;
  int request_table_ref;
  int response_table_ref;
} cai_lua_headers_ctx;

typedef struct cai_lua_tool_event_ctx {
  lua_State *L;
  int callback_ref;
  const cai_tool_event *current_event;
} cai_lua_tool_event_ctx;

typedef struct cai_lua_function_call_ctx {
  lua_State *L;
  int delta_ref;
  int done_ref;
} cai_lua_function_call_ctx;

static int cai_lua_make_sink(lua_State *L, int index, cai_lua_sink_ctx *ctx,
                             cai_sink **out, cai_error *error);
static int cai_lua_bool_result(lua_State *L, int rc, cai_error *error);
static int cai_lua_write_stack_json_or_stream(lua_State *L, int index,
                                              cai_sink *output,
                                              cai_error *error);

static int cai_lua_tool_event_write_output_lua(lua_State *L) {
  cai_lua_tool_event_ctx *event_ctx;
  const cai_tool_event *event;
  cai_lua_sink_ctx sink_ctx;
  cai_sink *sink;
  cai_error error;
  int rc;

  event_ctx = (cai_lua_tool_event_ctx *)lua_touserdata(L, lua_upvalueindex(1));
  event = event_ctx != NULL ? event_ctx->current_event : NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  sink = NULL;
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 1, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_tool_event_write_output(event, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static void cai_lua_error_cleanup(cai_error *error) {
  cai_error_cleanup(error);
}

static void cai_lua_push_error(lua_State *L, int rc, const cai_error *error) {
  lua_newtable(L);
  lua_pushinteger(L, rc);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, cai_status_string(rc));
  lua_setfield(L, -2, "status_string");
  if (error != NULL) {
    lua_pushinteger(L, error->code);
    lua_setfield(L, -2, "code");
    lua_pushinteger(L, error->http_status);
    lua_setfield(L, -2, "http_status");
    if (error->message != NULL) {
      lua_pushstring(L, error->message);
      lua_setfield(L, -2, "message");
    }
    if (error->detail != NULL) {
      lua_pushstring(L, error->detail);
      lua_setfield(L, -2, "detail");
    }
    if (error->server_code != NULL) {
      lua_pushstring(L, error->server_code);
      lua_setfield(L, -2, "server_code");
    }
    if (error->request_id != NULL) {
      lua_pushstring(L, error->request_id);
      lua_setfield(L, -2, "request_id");
    }
  }
}

static int cai_lua_fail(lua_State *L, int rc, cai_error *error) {
  lua_pushnil(L);
  cai_lua_push_error(L, rc, error);
  cai_lua_error_cleanup(error);
  return 2;
}

static int cai_lua_set_error(cai_error *error, int code, const char *message) {
  if (error != NULL) {
    cai_error_cleanup(error);
    error->code = code;
    if (message != NULL) {
      error->message = (char *)malloc(strlen(message) + 1u);
      if (error->message != NULL) {
        strcpy(error->message, message);
      }
    }
  }
  return code;
}

static int cai_lua_set_error_detail(cai_error *error, int code,
                                    const char *message, const char *detail) {
  cai_lua_set_error(error, code, message);
  if (error != NULL && detail != NULL) {
    error->detail = (char *)malloc(strlen(detail) + 1u);
    if (error->detail != NULL) {
      strcpy(error->detail, detail);
    }
  }
  return code;
}

static int cai_lua_bool_result(lua_State *L, int rc, cai_error *error) {
  if (rc == CAI_OK) {
    lua_pushboolean(L, 1);
    cai_lua_error_cleanup(error);
    return 1;
  }
  return cai_lua_fail(L, rc, error);
}

static const char *cai_lua_opt_string_field(lua_State *L, int index,
                                            const char *name,
                                            const char *fallback) {
  const char *value;
  index = lua_absindex(L, index);
  value = fallback;
  lua_getfield(L, index, name);
  if (!lua_isnil(L, -1)) {
    value = luaL_checkstring(L, -1);
  }
  lua_pop(L, 1);
  return value;
}

static int cai_lua_opt_int_field(lua_State *L, int index, const char *name,
                                 int fallback) {
  int value;
  index = lua_absindex(L, index);
  value = fallback;
  lua_getfield(L, index, name);
  if (!lua_isnil(L, -1)) {
    value = (int)luaL_checkinteger(L, -1);
  }
  lua_pop(L, 1);
  return value;
}

static long cai_lua_opt_long_field(lua_State *L, int index, const char *name,
                                   long fallback) {
  long value;
  index = lua_absindex(L, index);
  value = fallback;
  lua_getfield(L, index, name);
  if (!lua_isnil(L, -1)) {
    value = (long)luaL_checkinteger(L, -1);
  }
  lua_pop(L, 1);
  return value;
}

static long long cai_lua_opt_ll_field(lua_State *L, int index, const char *name,
                                      long long fallback) {
  long long value;
  index = lua_absindex(L, index);
  value = fallback;
  lua_getfield(L, index, name);
  if (!lua_isnil(L, -1)) {
    value = (long long)luaL_checkinteger(L, -1);
  }
  lua_pop(L, 1);
  return value;
}

static size_t cai_lua_opt_size_field(lua_State *L, int index, const char *name,
                                     size_t fallback) {
  lua_Integer n;
  index = lua_absindex(L, index);
  lua_getfield(L, index, name);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return fallback;
  }
  n = luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  if (n < 0) {
    luaL_error(L, "%s must be non-negative", name);
  }
  return (size_t)n;
}

static cai_lua_client *cai_lua_check_client(lua_State *L, int index) {
  cai_lua_client *self;
  self = (cai_lua_client *)luaL_checkudata(L, index, CAI_LUA_CLIENT);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai client");
  return self;
}

static cai_lua_agent *cai_lua_check_agent(lua_State *L, int index) {
  cai_lua_agent *self;
  self = (cai_lua_agent *)luaL_checkudata(L, index, CAI_LUA_AGENT);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai agent");
  return self;
}

static cai_lua_session *cai_lua_check_session(lua_State *L, int index) {
  cai_lua_session *self;
  self = (cai_lua_session *)luaL_checkudata(L, index, CAI_LUA_SESSION);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai session");
  return self;
}

static cai_lua_response *cai_lua_check_response(lua_State *L, int index) {
  cai_lua_response *self;
  self = (cai_lua_response *)luaL_checkudata(L, index, CAI_LUA_RESPONSE);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai response");
  return self;
}

static cai_lua_output *cai_lua_check_output(lua_State *L, int index) {
  cai_lua_output *self;
  self = (cai_lua_output *)luaL_checkudata(L, index, CAI_LUA_OUTPUT);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai output");
  return self;
}

static cai_lua_registry *cai_lua_check_registry(lua_State *L, int index) {
  cai_lua_registry *self;
  self = (cai_lua_registry *)luaL_checkudata(L, index, CAI_LUA_REGISTRY);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai tool registry");
  return self;
}

static cai_lua_mcp *cai_lua_check_mcp(lua_State *L, int index) {
  cai_lua_mcp *self;
  self = (cai_lua_mcp *)luaL_checkudata(L, index, CAI_LUA_MCP);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai mcp handler");
  return self;
}

static cai_lua_schema *cai_lua_check_schema(lua_State *L, int index) {
  cai_lua_schema *self;
  self = (cai_lua_schema *)luaL_checkudata(L, index, CAI_LUA_SCHEMA);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai tool schema");
  return self;
}

static cai_lua_params *cai_lua_check_params(lua_State *L, int index) {
  cai_lua_params *self;
  self = (cai_lua_params *)luaL_checkudata(L, index, CAI_LUA_PARAMS);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai response params");
  return self;
}

static cai_lua_conversation *cai_lua_check_conversation(lua_State *L,
                                                        int index) {
  cai_lua_conversation *self;
  self =
      (cai_lua_conversation *)luaL_checkudata(L, index, CAI_LUA_CONVERSATION);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai conversation");
  return self;
}

static cai_lua_input_items *cai_lua_check_input_items(lua_State *L, int index) {
  cai_lua_input_items *self;
  self = (cai_lua_input_items *)luaL_checkudata(L, index, CAI_LUA_INPUT_ITEMS);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai input item list");
  return self;
}

static cai_lua_conversation_item *cai_lua_check_conversation_item(lua_State *L,
                                                                  int index) {
  cai_lua_conversation_item *self;
  self = (cai_lua_conversation_item *)luaL_checkudata(
      L, index, CAI_LUA_CONVERSATION_ITEM);
  luaL_argcheck(L, self->ptr != NULL, index, "closed cai conversation item");
  return self;
}

static cai_lua_conversation_params *
cai_lua_check_conversation_params(lua_State *L, int index) {
  cai_lua_conversation_params *self;
  self = (cai_lua_conversation_params *)luaL_checkudata(
      L, index, CAI_LUA_CONVERSATION_PARAMS);
  luaL_argcheck(L, self->ptr != NULL, index,
                "closed cai conversation items params");
  return self;
}

static cai_lua_spool_reader *cai_lua_check_spool_reader(lua_State *L,
                                                        int index) {
  cai_lua_spool_reader *self;
  self =
      (cai_lua_spool_reader *)luaL_checkudata(L, index, CAI_LUA_SPOOL_READER);
  return self;
}

static void cai_lua_unref_tools(lua_State *L, cai_lua_tool_ref *tool) {
  cai_lua_tool_ref *next;
  while (tool != NULL) {
    next = tool->next;
    luaL_unref(L, LUA_REGISTRYINDEX, tool->callback_ref);
    free(tool->context);
    free(tool);
    tool = next;
  }
}

static void cai_lua_push_client(lua_State *L, cai_client *client) {
  cai_lua_client *ud;
  ud = (cai_lua_client *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = client;
  luaL_getmetatable(L, CAI_LUA_CLIENT);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_agent(lua_State *L, cai_agent *agent) {
  cai_lua_agent *ud;
  ud = (cai_lua_agent *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = agent;
  ud->L = L;
  ud->tools = NULL;
  luaL_getmetatable(L, CAI_LUA_AGENT);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_session(lua_State *L, cai_session *session) {
  cai_lua_session *ud;
  ud = (cai_lua_session *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = session;
  luaL_getmetatable(L, CAI_LUA_SESSION);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_response(lua_State *L, cai_response *response) {
  cai_lua_response *ud;
  ud = (cai_lua_response *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = response;
  luaL_getmetatable(L, CAI_LUA_RESPONSE);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_output(lua_State *L, cai_output *output) {
  cai_lua_output *ud;
  ud = (cai_lua_output *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = output;
  luaL_getmetatable(L, CAI_LUA_OUTPUT);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_registry(lua_State *L, cai_tool_registry *registry) {
  cai_lua_registry *ud;
  ud = (cai_lua_registry *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = registry;
  ud->L = L;
  ud->tools = NULL;
  luaL_getmetatable(L, CAI_LUA_REGISTRY);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_mcp(lua_State *L, cai_mcp_handler *handler) {
  cai_lua_mcp *ud;
  ud = (cai_lua_mcp *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = handler;
  luaL_getmetatable(L, CAI_LUA_MCP);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_schema(lua_State *L, cai_tool_schema *schema) {
  cai_lua_schema *ud;
  ud = (cai_lua_schema *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = schema;
  luaL_getmetatable(L, CAI_LUA_SCHEMA);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_params(lua_State *L, cai_response_create_params *ptr) {
  cai_lua_params *ud;
  ud = (cai_lua_params *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = ptr;
  luaL_getmetatable(L, CAI_LUA_PARAMS);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_conversation(lua_State *L, cai_conversation *ptr) {
  cai_lua_conversation *ud;
  ud = (cai_lua_conversation *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = ptr;
  luaL_getmetatable(L, CAI_LUA_CONVERSATION);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_input_items(lua_State *L, cai_input_item_list *ptr) {
  cai_lua_input_items *ud;
  ud = (cai_lua_input_items *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = ptr;
  luaL_getmetatable(L, CAI_LUA_INPUT_ITEMS);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_conversation_item(lua_State *L,
                                           cai_conversation_item *ptr) {
  cai_lua_conversation_item *ud;
  ud = (cai_lua_conversation_item *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = ptr;
  luaL_getmetatable(L, CAI_LUA_CONVERSATION_ITEM);
  lua_setmetatable(L, -2);
}

static void
cai_lua_push_conversation_params(lua_State *L,
                                 cai_conversation_items_params *ptr) {
  cai_lua_conversation_params *ud;
  ud = (cai_lua_conversation_params *)lua_newuserdata(L, sizeof(*ud));
  ud->ptr = ptr;
  luaL_getmetatable(L, CAI_LUA_CONVERSATION_PARAMS);
  lua_setmetatable(L, -2);
}

static void cai_lua_push_spool_reader(lua_State *L,
                                      const lonejson_spooled *spool) {
  cai_lua_spool_reader *ud;
  ud = (cai_lua_spool_reader *)lua_newuserdata(L, sizeof(*ud));
  memset(ud, 0, sizeof(*ud));
  if (spool != NULL) {
    ud->cursor = *spool;
  }
  ud->owns_cursor = 0;
  luaL_getmetatable(L, CAI_LUA_SPOOL_READER);
  lua_setmetatable(L, -2);
}

static int cai_lua_write_escaped_string(lua_State *L, luaL_Buffer *buffer,
                                        const char *s, size_t len) {
  size_t i;
  char tmp[7];
  (void)L;
  luaL_addchar(buffer, '"');
  for (i = 0u; i < len; i++) {
    unsigned char c;
    c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      luaL_addchar(buffer, '\\');
      luaL_addchar(buffer, (char)c);
    } else if (c == '\b') {
      luaL_addstring(buffer, "\\b");
    } else if (c == '\f') {
      luaL_addstring(buffer, "\\f");
    } else if (c == '\n') {
      luaL_addstring(buffer, "\\n");
    } else if (c == '\r') {
      luaL_addstring(buffer, "\\r");
    } else if (c == '\t') {
      luaL_addstring(buffer, "\\t");
    } else if (c < 0x20u) {
      snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
      luaL_addstring(buffer, tmp);
    } else {
      luaL_addchar(buffer, (char)c);
    }
  }
  luaL_addchar(buffer, '"');
  return 1;
}

static int cai_lua_encode_json_value(lua_State *L, luaL_Buffer *buffer,
                                     int index, int depth);

static int cai_lua_is_array(lua_State *L, int index, size_t *count) {
  size_t n;
  size_t seen;
  int is_array;
  index = lua_absindex(L, index);
  n = lua_rawlen(L, index);
  seen = 0u;
  is_array = 1;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    if (lua_type(L, -2) == LUA_TNUMBER) {
      lua_Integer k;
      k = lua_tointeger(L, -2);
      if (k < 1 || (size_t)k > n) {
        is_array = 0;
      }
    } else {
      is_array = 0;
    }
    seen++;
    lua_pop(L, 1);
  }
  if (seen != n) {
    is_array = 0;
  }
  *count = n;
  return is_array;
}

static int cai_lua_encode_table(lua_State *L, luaL_Buffer *buffer, int index,
                                int depth) {
  size_t count;
  size_t i;
  int first;
  index = lua_absindex(L, index);
  if (cai_lua_is_array(L, index, &count)) {
    luaL_addchar(buffer, '[');
    for (i = 1u; i <= count; i++) {
      if (i > 1u) {
        luaL_addchar(buffer, ',');
      }
      lua_rawgeti(L, index, (lua_Integer)i);
      cai_lua_encode_json_value(L, buffer, -1, depth + 1);
      lua_pop(L, 1);
    }
    luaL_addchar(buffer, ']');
    return 1;
  }
  luaL_addchar(buffer, '{');
  first = 1;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    size_t key_len;
    const char *key;
    if (lua_type(L, -2) != LUA_TSTRING) {
      return luaL_error(L, "JSON object keys must be strings");
    }
    if (!first) {
      luaL_addchar(buffer, ',');
    }
    first = 0;
    key = lua_tolstring(L, -2, &key_len);
    cai_lua_write_escaped_string(L, buffer, key, key_len);
    luaL_addchar(buffer, ':');
    cai_lua_encode_json_value(L, buffer, -1, depth + 1);
    lua_pop(L, 1);
  }
  luaL_addchar(buffer, '}');
  return 1;
}

static int cai_lua_encode_json_value(lua_State *L, luaL_Buffer *buffer,
                                     int index, int depth) {
  int type;
  size_t len;
  const char *s;
  char num[64];
  if (depth > 64) {
    return luaL_error(L, "JSON nesting too deep");
  }
  type = lua_type(L, index);
  switch (type) {
  case LUA_TNIL:
    luaL_addstring(buffer, "null");
    break;
  case LUA_TBOOLEAN:
    luaL_addstring(buffer, lua_toboolean(L, index) ? "true" : "false");
    break;
  case LUA_TNUMBER:
    snprintf(num, sizeof(num), "%.17g", (double)lua_tonumber(L, index));
    luaL_addstring(buffer, num);
    break;
  case LUA_TSTRING:
    s = lua_tolstring(L, index, &len);
    cai_lua_write_escaped_string(L, buffer, s, len);
    break;
  case LUA_TTABLE:
    cai_lua_encode_table(L, buffer, index, depth);
    break;
  default:
    return luaL_error(L, "cannot encode %s as JSON", lua_typename(L, type));
  }
  return 1;
}

static const char *cai_lua_json_from_stack(lua_State *L, int index,
                                           size_t *len) {
  index = lua_absindex(L, index);
  if (lua_type(L, index) == LUA_TSTRING) {
    return lua_tolstring(L, index, len);
  }
  {
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    cai_lua_encode_json_value(L, &buffer, index, 0);
    luaL_pushresult(&buffer);
    return lua_tolstring(L, -1, len);
  }
}

static int cai_lua_stack_has_read_method(lua_State *L, int index) {
  int has_read;
  index = lua_absindex(L, index);
  if (lua_type(L, index) != LUA_TTABLE && lua_type(L, index) != LUA_TUSERDATA) {
    return 0;
  }
  lua_getfield(L, index, "read");
  has_read = lua_isfunction(L, -1);
  lua_pop(L, 1);
  return has_read;
}

static int cai_lua_call_optional_rewind(lua_State *L, int index,
                                        cai_error *error) {
  int rc;
  index = lua_absindex(L, index);
  lua_getfield(L, index, "rewind");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return CAI_OK;
  }
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return cai_lua_set_error(error, CAI_ERR_INVALID,
                             "spooled value rewind member is not callable");
  }
  lua_pushvalue(L, index);
  rc = lua_pcall(L, 1, 2, 0);
  if (rc != LUA_OK) {
    lua_pop(L, 1);
    return cai_lua_set_error(error, CAI_ERR_INVALID,
                             "failed to rewind Lua spooled value");
  }
  if (lua_isnil(L, -2)) {
    lua_pop(L, 2);
    return cai_lua_set_error(error, CAI_ERR_INVALID,
                             "Lua spooled value rewind failed");
  }
  lua_pop(L, 2);
  return CAI_OK;
}

static int cai_lua_stream_lua_reader_to_sink(lua_State *L, int index,
                                             cai_sink *sink, cai_error *error) {
  int rc;
  int empty_reads;
  index = lua_absindex(L, index);
  if (sink == NULL) {
    return cai_lua_set_error(error, CAI_ERR_INVALID, "sink is required");
  }
  rc = cai_lua_call_optional_rewind(L, index, error);
  if (rc != CAI_OK) {
    return rc;
  }
  empty_reads = 0;
  for (;;) {
    size_t len;
    const char *chunk;

    lua_getfield(L, index, "read");
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      return cai_lua_set_error(error, CAI_ERR_INVALID,
                               "spooled value read member is not callable");
    }
    lua_pushvalue(L, index);
    lua_pushinteger(L, 4096);
    rc = lua_pcall(L, 2, 1, 0);
    if (rc != LUA_OK) {
      lua_pop(L, 1);
      return cai_lua_set_error(error, CAI_ERR_INVALID,
                               "failed to read Lua spooled value");
    }
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      return CAI_OK;
    }
    chunk = luaL_checklstring(L, -1, &len);
    if (len == 0U) {
      empty_reads++;
      lua_pop(L, 1);
      if (empty_reads > 16) {
        return cai_lua_set_error(
            error, CAI_ERR_INVALID,
            "Lua spooled value returned too many empty reads");
      }
      continue;
    }
    empty_reads = 0;
    rc = cai_sink_write(sink, chunk, len, error);
    lua_pop(L, 1);
    if (rc != CAI_OK) {
      return rc;
    }
  }
}

static int cai_lua_stream_function_to_sink(lua_State *L, int index,
                                           cai_sink *sink, cai_error *error) {
  int rc;
  int empty_reads;
  index = lua_absindex(L, index);
  empty_reads = 0;
  for (;;) {
    size_t len;
    const char *chunk;

    lua_pushvalue(L, index);
    rc = lua_pcall(L, 0, 1, 0);
    if (rc != LUA_OK) {
      lua_pop(L, 1);
      return cai_lua_set_error(error, CAI_ERR_INVALID,
                               "failed to read Lua source function");
    }
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      return CAI_OK;
    }
    chunk = luaL_checklstring(L, -1, &len);
    if (len == 0U) {
      empty_reads++;
      lua_pop(L, 1);
      if (empty_reads > 16) {
        return cai_lua_set_error(error, CAI_ERR_INVALID,
                                 "Lua source returned too many empty reads");
      }
      continue;
    }
    empty_reads = 0;
    rc = cai_sink_write(sink, chunk, len, error);
    lua_pop(L, 1);
    if (rc != CAI_OK) {
      return rc;
    }
  }
}

static int cai_lua_spooled_append_sink(void *context, const void *bytes,
                                       size_t count, cai_error *error) {
  lonejson_spooled *spool;
  lonejson_error json_error;

  spool = (lonejson_spooled *)context;
  if (count == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(spool, bytes, count, &json_error) ==
      LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_lua_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to append Lua spooled value",
                                  json_error.message);
}

static int cai_lua_spool_from_stack(lua_State *L, int index,
                                    lonejson_spooled *out, cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  lonejson_error json_error;
  int rc;
  size_t len;
  const char *text;

  memset(out, 0, sizeof(*out));
  lonejson_spooled_init(out, NULL);
  sink = NULL;
  callbacks.write = cai_lua_spooled_append_sink;
  callbacks.close = NULL;
  callbacks.context = out;
  rc = cai_sink_from_callbacks(&callbacks, &sink, error);
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(out);
    return rc;
  }

  index = lua_absindex(L, index);
  if (lua_type(L, index) == LUA_TSTRING) {
    text = lua_tolstring(L, index, &len);
    lonejson_error_init(&json_error);
    if (lonejson_spooled_append(out, text, len, &json_error) !=
        LONEJSON_STATUS_OK) {
      cai_sink_close(sink);
      lonejson_spooled_cleanup(out);
      return cai_lua_set_error_detail(error, CAI_ERR_TRANSPORT,
                                      "failed to append Lua string to spool",
                                      json_error.message);
    }
    cai_sink_close(sink);
    return CAI_OK;
  }
  if (lua_type(L, index) == LUA_TFUNCTION) {
    rc = cai_lua_stream_function_to_sink(L, index, sink, error);
  } else if (cai_lua_stack_has_read_method(L, index)) {
    rc = cai_lua_stream_lua_reader_to_sink(L, index, sink, error);
  } else {
    rc = cai_lua_set_error(
        error, CAI_ERR_INVALID,
        "expected string, source callback, or spooled reader");
  }
  cai_sink_close(sink);
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(out);
  }
  return rc;
}

static int cai_lua_write_stack_json_or_stream(lua_State *L, int index,
                                              cai_sink *output,
                                              cai_error *error) {
  size_t len;
  const char *json;
  int rc;

  index = lua_absindex(L, index);
  if (lua_type(L, index) == LUA_TFUNCTION) {
    return cai_lua_stream_function_to_sink(L, index, output, error);
  }
  if (cai_lua_stack_has_read_method(L, index)) {
    return cai_lua_stream_lua_reader_to_sink(L, index, output, error);
  }
  json = cai_lua_json_from_stack(L, index, &len);
  rc = cai_sink_write(output, json, len, error);
  if (!lua_isstring(L, index) && lua_type(L, -1) == LUA_TSTRING) {
    lua_pop(L, 1);
  }
  return rc;
}

static int cai_lua_sink_write(void *context, const void *bytes, size_t count,
                              cai_error *error) {
  cai_lua_sink_ctx *ctx;
  int rc;
  (void)error;
  ctx = (cai_lua_sink_ctx *)context;
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
  lua_pushlstring(ctx->L, (const char *)bytes, count);
  rc = lua_pcall(ctx->L, 1, 1, 0);
  if (rc != LUA_OK) {
    return CAI_ERR_INVALID;
  }
  if (lua_isboolean(ctx->L, -1) && !lua_toboolean(ctx->L, -1)) {
    lua_pop(ctx->L, 1);
    return CAI_ERR_CANCELLED;
  }
  lua_pop(ctx->L, 1);
  return CAI_OK;
}

static void cai_lua_sink_close(void *context) { (void)context; }

static int cai_lua_make_sink(lua_State *L, int index, cai_lua_sink_ctx *ctx,
                             cai_sink **out, cai_error *error) {
  cai_sink_callbacks callbacks;
  index = lua_absindex(L, index);
  ctx->L = L;
  luaL_checktype(L, index, LUA_TFUNCTION);
  lua_pushvalue(L, index);
  ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  callbacks.write = cai_lua_sink_write;
  callbacks.close = cai_lua_sink_close;
  callbacks.context = ctx;
  return cai_sink_from_callbacks(&callbacks, out, error);
}

static int cai_lua_call_chunk_source(cai_lua_source_ctx *ctx) {
  int rc;
  if (ctx->done) {
    return 0;
  }
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
  rc = lua_pcall(ctx->L, 0, 1, 0);
  if (rc != LUA_OK) {
    ctx->done = 1;
    return 0;
  }
  if (lua_isnil(ctx->L, -1)) {
    ctx->done = 1;
    lua_pop(ctx->L, 1);
    return 0;
  }
  {
    size_t len;
    const char *chunk;
    chunk = luaL_checklstring(ctx->L, -1, &len);
    free(ctx->chunk);
    ctx->chunk = (char *)malloc(len == 0u ? 1u : len);
    if (ctx->chunk == NULL) {
      ctx->done = 1;
      lua_pop(ctx->L, 1);
      return 0;
    }
    memcpy(ctx->chunk, chunk, len);
    ctx->chunk_len = len;
    ctx->chunk_pos = 0u;
  }
  lua_pop(ctx->L, 1);
  return 1;
}

static size_t cai_lua_source_read(void *context, void *buffer, size_t count,
                                  cai_error *error) {
  cai_lua_source_ctx *ctx;
  size_t avail;
  (void)error;
  ctx = (cai_lua_source_ctx *)context;
  if (ctx->bytes != NULL) {
    if (ctx->pos >= ctx->len) {
      return 0u;
    }
    avail = ctx->len - ctx->pos;
    if (avail > count) {
      avail = count;
    }
    memcpy(buffer, ctx->bytes + ctx->pos, avail);
    ctx->pos += avail;
    return avail;
  }
  while (ctx->chunk_pos >= ctx->chunk_len) {
    if (!cai_lua_call_chunk_source(ctx)) {
      return 0u;
    }
  }
  avail = ctx->chunk_len - ctx->chunk_pos;
  if (avail > count) {
    avail = count;
  }
  memcpy(buffer, ctx->chunk + ctx->chunk_pos, avail);
  ctx->chunk_pos += avail;
  return avail;
}

static int cai_lua_source_reset(void *context, cai_error *error) {
  cai_lua_source_ctx *ctx;
  (void)error;
  ctx = (cai_lua_source_ctx *)context;
  if (ctx->bytes != NULL) {
    ctx->pos = 0u;
    return CAI_OK;
  }
  return CAI_ERR_INVALID;
}

static void cai_lua_source_close(void *context) {
  cai_lua_source_ctx *ctx;
  ctx = (cai_lua_source_ctx *)context;
  free(ctx->chunk);
}

static int cai_lua_make_source(lua_State *L, int index, cai_lua_source_ctx *ctx,
                               cai_source **out, cai_error *error) {
  cai_source_callbacks callbacks;
  size_t len;
  index = lua_absindex(L, index);
  memset(ctx, 0, sizeof(*ctx));
  ctx->L = L;
  if (lua_type(L, index) == LUA_TSTRING) {
    ctx->bytes = lua_tolstring(L, index, &len);
    ctx->len = len;
  } else {
    luaL_checktype(L, index, LUA_TFUNCTION);
    lua_pushvalue(L, index);
    ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  callbacks.read = cai_lua_source_read;
  callbacks.reset = cai_lua_source_reset;
  callbacks.close = cai_lua_source_close;
  callbacks.context = ctx;
  return cai_source_from_callbacks(&callbacks, out, error);
}

static int cai_lua_copy_source_to_callback(lua_State *L, cai_source *source,
                                           int callback_index,
                                           cai_error *error) {
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  int rc;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  sink = NULL;
  rc = cai_lua_make_sink(L, callback_index, &sink_ctx, &sink, error);
  if (rc == CAI_OK) {
    rc = cai_source_copy_to_sink(source, sink, error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return rc;
}

static void cai_lua_list_params_from_table(lua_State *L, int index,
                                           cai_list_params *params) {
  index = lua_absindex(L, index);
  cai_list_params_init(params);
  if (!lua_istable(L, index)) {
    return;
  }
  params->after = cai_lua_opt_string_field(L, index, "after", params->after);
  params->limit = cai_lua_opt_int_field(L, index, "limit", params->limit);
  params->order = cai_lua_opt_string_field(L, index, "order", params->order);
}

static int cai_lua_call_tool(lua_State *L, int callback_ref,
                             const char *arguments_json, cai_sink *output,
                             cai_error *error) {
  int rc;
  lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
  lua_pushstring(L, arguments_json != NULL ? arguments_json : "{}");
  rc = lua_pcall(L, 1, 1, 0);
  if (rc != LUA_OK) {
    if (error != NULL) {
      error->code = CAI_ERR_INVALID;
    }
    lua_pop(L, 1);
    return CAI_ERR_INVALID;
  }
  rc = cai_lua_write_stack_json_or_stream(L, -1, output, error);
  lua_pop(L, 1);
  return rc;
}

typedef struct cai_lua_tool_context {
  lua_State *L;
  int callback_ref;
} cai_lua_tool_context;

static int cai_lua_raw_tool_trampoline(void *context,
                                       const char *arguments_json,
                                       cai_sink *output, cai_error *error) {
  cai_lua_tool_context *ctx;
  ctx = (cai_lua_tool_context *)context;
  return cai_lua_call_tool(ctx->L, ctx->callback_ref, arguments_json, output,
                           error);
}

static int cai_lua_raw_spooled_tool_trampoline(void *context,
                                               lonejson_spooled *arguments_json,
                                               cai_sink *output,
                                               cai_error *error) {
  cai_lua_tool_context *ctx;
  int rc;
  ctx = (cai_lua_tool_context *)context;
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
  cai_lua_push_spool_reader(ctx->L, arguments_json);
  rc = lua_pcall(ctx->L, 1, 1, 0);
  if (rc != LUA_OK) {
    lua_pop(ctx->L, 1);
    return cai_lua_set_error(error, CAI_ERR_INVALID,
                             "Lua spooled tool callback failed");
  }
  rc = cai_lua_write_stack_json_or_stream(ctx->L, -1, output, error);
  lua_pop(ctx->L, 1);
  return rc;
}

static void cai_lua_agent_config_from_table(lua_State *L, int index,
                                            cai_agent_config *config) {
  int p;
  index = lua_absindex(L, index);
  cai_agent_config_init(config);
  if (!lua_istable(L, index)) {
    return;
  }
  config->model = cai_lua_opt_string_field(L, index, "model", config->model);
  config->developer_instructions = cai_lua_opt_string_field(
      L, index, "developer_instructions", config->developer_instructions);
  config->developer_instructions = cai_lua_opt_string_field(
      L, index, "instructions", config->developer_instructions);
  config->prompt_cache_key = cai_lua_opt_string_field(
      L, index, "prompt_cache_key", config->prompt_cache_key);
  config->tool_choice =
      cai_lua_opt_string_field(L, index, "tool_choice", config->tool_choice);
  config->reasoning_effort = cai_lua_opt_string_field(
      L, index, "reasoning_effort", config->reasoning_effort);
  config->reasoning_summary = cai_lua_opt_string_field(
      L, index, "reasoning_summary", config->reasoning_summary);
  config->max_output_tokens = cai_lua_opt_int_field(
      L, index, "max_output_tokens", config->max_output_tokens);
  config->parallel_tool_calls = cai_lua_opt_int_field(
      L, index, "parallel_tool_calls", config->parallel_tool_calls);
  config->session_continuity = cai_lua_opt_int_field(
      L, index, "session_continuity", config->session_continuity);
  config->disable_auto_compaction = cai_lua_opt_int_field(
      L, index, "disable_auto_compaction", config->disable_auto_compaction);
  config->compact_threshold_tokens = cai_lua_opt_ll_field(
      L, index, "compact_threshold_tokens", config->compact_threshold_tokens);
  p = cai_lua_opt_int_field(L, index, "compact_threshold_percent",
                            (int)config->compact_threshold_percent);
  if (p < 0) {
    luaL_error(L, "compact_threshold_percent must be non-negative");
  }
  config->compact_threshold_percent = (unsigned int)p;
  config->history_memory_limit = cai_lua_opt_size_field(
      L, index, "history_memory_limit", config->history_memory_limit);
  config->history_spool_dir = cai_lua_opt_string_field(
      L, index, "history_spool_dir", config->history_spool_dir);
}

static int cai_lua_open(lua_State *L) {
  cai_client_config config;
  cai_client *client;
  cai_error error;
  int rc;
  cai_error_init(&error);
  cai_client_config_init(&config);
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "openrouter");
    if (lua_toboolean(L, -1)) {
      cai_client_config_use_openrouter(&config);
    }
    lua_pop(L, 1);
    config.api_key = cai_lua_opt_string_field(L, 1, "api_key", config.api_key);
    config.api_key_env =
        cai_lua_opt_string_field(L, 1, "api_key_env", config.api_key_env);
    config.base_url =
        cai_lua_opt_string_field(L, 1, "base_url", config.base_url);
    config.organization_id = cai_lua_opt_string_field(L, 1, "organization_id",
                                                      config.organization_id);
    config.project_id =
        cai_lua_opt_string_field(L, 1, "project_id", config.project_id);
    config.timeout_ms =
        cai_lua_opt_long_field(L, 1, "timeout_ms", config.timeout_ms);
    config.http_2_disabled =
        cai_lua_opt_int_field(L, 1, "http_2_disabled", config.http_2_disabled);
    config.insecure_skip_verify = cai_lua_opt_int_field(
        L, 1, "insecure_skip_verify", config.insecure_skip_verify);
    config.json_response_limit_bytes = cai_lua_opt_size_field(
        L, 1, "json_response_limit_bytes", config.json_response_limit_bytes);
  }
  rc = cai_client_open(&config, &client, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_client(L, client);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_load_dotenv_api_key(lua_State *L) {
  const char *dotenv_path;
  const char *api_key_env;
  cai_error error;
  char *api_key;
  int rc;

  dotenv_path = luaL_checkstring(L, 1);
  api_key_env = luaL_optstring(L, 2, NULL);
  api_key = NULL;
  cai_error_init(&error);
  rc = cai_load_dotenv_api_key(dotenv_path, api_key_env, &api_key, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  lua_pushstring(L, api_key);
  cai_string_destroy(api_key);
  return 1;
}

static int cai_lua_client_gc(lua_State *L) {
  cai_lua_client *self;
  self = (cai_lua_client *)luaL_checkudata(L, 1, CAI_LUA_CLIENT);
  if (self->ptr != NULL) {
    cai_client_close(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_client_close(lua_State *L) { return cai_lua_client_gc(L); }

static int cai_lua_client_new_agent(lua_State *L) {
  cai_lua_client *self;
  cai_agent_config config;
  cai_agent *agent;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  cai_lua_agent_config_from_table(L, 2, &config);
  rc = cai_client_new_agent(self->ptr, &config, &agent, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_agent(L, agent);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_create_conversation(lua_State *L) {
  cai_lua_client *self;
  cai_conversation *conversation;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  rc = cai_client_create_conversation(self->ptr, &conversation, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_conversation(L, conversation);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_retrieve_conversation(lua_State *L) {
  cai_lua_client *self;
  cai_conversation *conversation;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *handle;
    handle = cai_lua_check_conversation(L, 2);
    rc = cai_client_retrieve_conversation_handle(self->ptr, handle->ptr,
                                                 &conversation, &error);
  } else {
    rc = cai_client_retrieve_conversation(self->ptr, luaL_checkstring(L, 2),
                                          &conversation, &error);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_conversation(L, conversation);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_update_conversation_metadata(lua_State *L) {
  cai_lua_client *self;
  cai_conversation *conversation;
  cai_error error;
  size_t len;
  const char *metadata_json;
  int rc;
  self = cai_lua_check_client(L, 1);
  metadata_json = cai_lua_json_from_stack(L, 3, &len);
  (void)len;
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *handle;
    handle = cai_lua_check_conversation(L, 2);
    rc = cai_client_update_conversation_metadata_handle(
        self->ptr, handle->ptr, metadata_json, &conversation, &error);
  } else {
    rc = cai_client_update_conversation_metadata(
        self->ptr, luaL_checkstring(L, 2), metadata_json, &conversation,
        &error);
  }
  if (lua_type(L, -1) == LUA_TSTRING && !lua_isstring(L, 3)) {
    lua_pop(L, 1);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_conversation(L, conversation);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_delete_conversation(lua_State *L) {
  cai_lua_client *self;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *handle;
    handle = cai_lua_check_conversation(L, 2);
    rc = cai_client_delete_conversation_handle(self->ptr, handle->ptr, &error);
  } else {
    rc = cai_client_delete_conversation(self->ptr, luaL_checkstring(L, 2),
                                        &error);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_client_create_response(lua_State *L) {
  cai_lua_client *self;
  cai_lua_params *params;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  params = cai_lua_check_params(L, 2);
  cai_error_init(&error);
  rc = cai_client_create_response(self->ptr, params->ptr, &response, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_stream_response_text(lua_State *L) {
  cai_lua_client *self;
  cai_lua_params *params;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  params = cai_lua_check_params(L, 2);
  sink = NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 3, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_text(self->ptr, params->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_client_retrieve_response(lua_State *L) {
  cai_lua_client *self;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  rc = cai_client_retrieve_response(self->ptr, luaL_checkstring(L, 2),
                                    &response, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_cancel_response(lua_State *L) {
  cai_lua_client *self;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  rc = cai_client_cancel_response(self->ptr, luaL_checkstring(L, 2), &response,
                                  &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_delete_response(lua_State *L) {
  cai_lua_client *self;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  rc = cai_client_delete_response(self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_client_list_response_input_items(lua_State *L) {
  cai_lua_client *self;
  cai_input_item_list *list;
  cai_list_params params;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_lua_list_params_from_table(L, 3, &params);
  cai_error_init(&error);
  rc = cai_client_list_response_input_items(self->ptr, luaL_checkstring(L, 2),
                                            &params, &list, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_input_items(L, list);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_list_conversation_items(lua_State *L) {
  cai_lua_client *self;
  cai_input_item_list *list;
  cai_list_params params;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_lua_list_params_from_table(L, 3, &params);
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *conversation;
    conversation = cai_lua_check_conversation(L, 2);
    rc = cai_client_list_conversation_items_handle(self->ptr, conversation->ptr,
                                                   &params, &list, &error);
  } else {
    rc = cai_client_list_conversation_items(self->ptr, luaL_checkstring(L, 2),
                                            &params, &list, &error);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_input_items(L, list);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_create_conversation_items(lua_State *L) {
  cai_lua_client *self;
  cai_lua_conversation_params *params;
  cai_input_item_list *list;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  params = cai_lua_check_conversation_params(L, 3);
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *conversation;
    conversation = cai_lua_check_conversation(L, 2);
    rc = cai_client_create_conversation_items_handle(
        self->ptr, conversation->ptr, params->ptr, &list, &error);
  } else {
    rc = cai_client_create_conversation_items(self->ptr, luaL_checkstring(L, 2),
                                              params->ptr, &list, &error);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_input_items(L, list);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_retrieve_conversation_item(lua_State *L) {
  cai_lua_client *self;
  cai_conversation_item *item;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *conversation;
    conversation = cai_lua_check_conversation(L, 2);
    rc = cai_client_retrieve_conversation_item_handle(
        self->ptr, conversation->ptr, luaL_checkstring(L, 3), &item, &error);
  } else {
    rc = cai_client_retrieve_conversation_item(
        self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3), &item,
        &error);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_conversation_item(L, item);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_client_delete_conversation_item(lua_State *L) {
  cai_lua_client *self;
  cai_error error;
  int rc;
  self = cai_lua_check_client(L, 1);
  cai_error_init(&error);
  if (luaL_testudata(L, 2, CAI_LUA_CONVERSATION) != NULL) {
    cai_lua_conversation *conversation;
    conversation = cai_lua_check_conversation(L, 2);
    rc = cai_client_delete_conversation_item_handle(
        self->ptr, conversation->ptr, luaL_checkstring(L, 3), &error);
  } else {
    rc = cai_client_delete_conversation_item(self->ptr, luaL_checkstring(L, 2),
                                             luaL_checkstring(L, 3), &error);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_gc(lua_State *L) {
  cai_lua_agent *self;
  self = (cai_lua_agent *)luaL_checkudata(L, 1, CAI_LUA_AGENT);
  if (self->ptr != NULL) {
    cai_agent_destroy(self->ptr);
    self->ptr = NULL;
  }
  cai_lua_unref_tools(L, self->tools);
  self->tools = NULL;
  return 0;
}

static int cai_lua_agent_close(lua_State *L) { return cai_lua_agent_gc(L); }

static int cai_lua_agent_new_session(lua_State *L) {
  cai_lua_agent *self;
  cai_session *session;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = cai_agent_new_session(self->ptr, &session, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_session(L, session);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_new_conversation_session(lua_State *L) {
  cai_lua_agent *self;
  cai_session *session;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->new_conversation_session(self->ptr, &session, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_session(L, session);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_new_session_for_conversation(lua_State *L) {
  cai_lua_agent *self;
  cai_lua_conversation *conversation;
  cai_session *session;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  conversation = cai_lua_check_conversation(L, 2);
  cai_error_init(&error);
  rc = self->ptr->new_session_for_conversation(self->ptr, conversation->ptr,
                                               &session, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_session(L, session);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_add_user_text(lua_State *L) {
  cai_lua_agent *self;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->add_user_text(self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_add_user_text_source(lua_State *L) {
  cai_lua_agent *self;
  cai_source *source;
  cai_lua_source_ctx source_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  source = NULL;
  rc = cai_lua_make_source(L, 2, &source_ctx, &source, &error);
  if (rc == CAI_OK) {
    rc = self->ptr->add_user_text_source(self->ptr, source, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  if (source_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_add_user_text_spooled(lua_State *L) {
  cai_lua_agent *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 2, &spool, &error);
  if (rc == CAI_OK) {
    rc = self->ptr->add_user_text_spooled(self->ptr, &spool, &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_add_user_image_url(lua_State *L) {
  cai_lua_agent *self;
  cai_error error;
  const char *detail;
  int rc;
  self = cai_lua_check_agent(L, 1);
  detail = luaL_optstring(L, 3, NULL);
  cai_error_init(&error);
  rc = self->ptr->add_user_image_url(self->ptr, luaL_checkstring(L, 2), detail,
                                     &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_add_user_file_path(lua_State *L) {
  cai_lua_agent *self;
  cai_error error;
  const char *filename;
  const char *detail;
  int rc;
  self = cai_lua_check_agent(L, 1);
  filename = luaL_optstring(L, 3, NULL);
  detail = luaL_optstring(L, 4, NULL);
  cai_error_init(&error);
  rc = self->ptr->add_user_file_path(self->ptr, luaL_checkstring(L, 2),
                                     filename, detail, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_add_user_file_data_spooled(lua_State *L) {
  cai_lua_agent *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 3, &spool, &error);
  if (rc == CAI_OK) {
    rc = self->ptr->add_user_file_data_spooled(
        self->ptr, luaL_checkstring(L, 2), &spool, luaL_optstring(L, 4, NULL),
        &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_push_usage(lua_State *L, const cai_token_usage *usage) {
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)usage->input_tokens);
  lua_setfield(L, -2, "input_tokens");
  lua_pushinteger(L, (lua_Integer)usage->input_cached_tokens);
  lua_setfield(L, -2, "input_cached_tokens");
  lua_pushinteger(L, (lua_Integer)usage->output_tokens);
  lua_setfield(L, -2, "output_tokens");
  lua_pushinteger(L, (lua_Integer)usage->output_reasoning_tokens);
  lua_setfield(L, -2, "output_reasoning_tokens");
  lua_pushinteger(L, (lua_Integer)usage->total_tokens);
  lua_setfield(L, -2, "total_tokens");
  return 1;
}

static int cai_lua_agent_last_usage(lua_State *L) {
  cai_lua_agent *self;
  cai_token_usage usage;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->last_usage(self->ptr, &usage, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_usage(L, &usage);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_context_percent(lua_State *L) {
  cai_lua_agent *self;
  cai_error error;
  double percent;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->context_percent(self->ptr, &percent, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  lua_pushnumber(L, percent);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_run(lua_State *L) {
  cai_lua_agent *self;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->run(self->ptr, &response, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_tool_event_trampoline(void *context,
                                         const cai_tool_event *event,
                                         cai_error *error) {
  cai_lua_tool_event_ctx *ctx;
  int rc;
  (void)error;
  ctx = (cai_lua_tool_event_ctx *)context;
  ctx->current_event = event;
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
  lua_newtable(ctx->L);
  lua_pushinteger(ctx->L, event->type);
  lua_setfield(ctx->L, -2, "type");
  if (event->type == CAI_TOOL_EVENT_START) {
    lua_pushstring(ctx->L, "start");
  } else if (event->type == CAI_TOOL_EVENT_OUTPUT) {
    lua_pushstring(ctx->L, "output");
  } else if (event->type == CAI_TOOL_EVENT_ERROR) {
    lua_pushstring(ctx->L, "error");
  } else {
    lua_pushstring(ctx->L, "unknown");
  }
  lua_setfield(ctx->L, -2, "kind");
  if (event->name != NULL) {
    lua_pushstring(ctx->L, event->name);
    lua_setfield(ctx->L, -2, "name");
  }
  if (event->arguments_json != NULL) {
    lua_pushstring(ctx->L, event->arguments_json);
    lua_setfield(ctx->L, -2, "arguments_json");
  }
  if (event->arguments_json_spooled != NULL) {
    lua_pushinteger(ctx->L, (lua_Integer)event->arguments_json_spooled->size);
    lua_setfield(ctx->L, -2, "arguments_size");
    cai_lua_push_spool_reader(ctx->L, event->arguments_json_spooled);
    lua_setfield(ctx->L, -2, "arguments_spooled");
  }
  if (event->output_json != NULL) {
    lua_pushinteger(ctx->L, (lua_Integer)event->output_json->size);
    lua_setfield(ctx->L, -2, "output_size");
    lua_pushlightuserdata(ctx->L, ctx);
    lua_pushcclosure(ctx->L, cai_lua_tool_event_write_output_lua, 1);
    lua_setfield(ctx->L, -2, "write_output");
  }
  if (event->tool_error != NULL) {
    cai_lua_push_error(ctx->L, event->tool_error->code, event->tool_error);
    lua_setfield(ctx->L, -2, "error");
  }
  rc = lua_pcall(ctx->L, 1, 1, 0);
  if (rc != LUA_OK) {
    lua_pop(ctx->L, 1);
    ctx->current_event = NULL;
    return CAI_ERR_INVALID;
  }
  if (lua_isboolean(ctx->L, -1) && !lua_toboolean(ctx->L, -1)) {
    lua_pop(ctx->L, 1);
    ctx->current_event = NULL;
    return CAI_ERR_CANCELLED;
  }
  lua_pop(ctx->L, 1);
  ctx->current_event = NULL;
  return CAI_OK;
}

static void cai_lua_run_options_from_table(lua_State *L, int index,
                                           cai_run_options *options,
                                           cai_lua_tool_event_ctx *event_ctx) {
  index = lua_absindex(L, index);
  cai_run_options_init(options);
  if (!lua_istable(L, index)) {
    return;
  }
  options->max_tool_rounds = cai_lua_opt_int_field(L, index, "max_tool_rounds",
                                                   options->max_tool_rounds);
  options->tool_output_memory_limit = cai_lua_opt_size_field(
      L, index, "tool_output_memory_limit", options->tool_output_memory_limit);
  options->tool_output_max_bytes = cai_lua_opt_size_field(
      L, index, "tool_output_max_bytes", options->tool_output_max_bytes);
  options->tool_spool_dir = cai_lua_opt_string_field(L, index, "tool_spool_dir",
                                                     options->tool_spool_dir);
  lua_getfield(L, index, "tool_event");
  if (lua_isfunction(L, -1)) {
    event_ctx->L = L;
    lua_pushvalue(L, -1);
    event_ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    options->tool_event = cai_lua_tool_event_trampoline;
    options->tool_event_context = event_ctx;
  }
  lua_pop(L, 1);
}

static int cai_lua_agent_run_auto(lua_State *L) {
  cai_lua_agent *self;
  cai_response *response;
  cai_run_options options;
  cai_lua_tool_event_ctx event_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  memset(&event_ctx, 0, sizeof(event_ctx));
  cai_error_init(&error);
  cai_lua_run_options_from_table(L, 2, &options, &event_ctx);
  rc = self->ptr->run_auto(self->ptr, &options, &response, &error);
  if (event_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, event_ctx.callback_ref);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_run_output(lua_State *L) {
  cai_lua_agent *self;
  cai_output *output;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->run_output(self->ptr, &output, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_output(L, output);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_send_text(lua_State *L) {
  cai_lua_agent *self;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  rc = self->ptr->send_text(self->ptr, luaL_checkstring(L, 2), &response,
                            &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_agent_stream_text(lua_State *L) {
  cai_lua_agent *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_error_init(&error);
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  sink = NULL;
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = self->ptr->stream_text(self->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_stream_output_delta(void *context, const char *item_id,
                                       int output_index, const char *delta,
                                       cai_error *error) {
  cai_lua_sink_ctx *ctx;
  int rc;
  (void)item_id;
  (void)output_index;
  (void)error;
  ctx = (cai_lua_sink_ctx *)context;
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
  lua_pushstring(ctx->L, delta != NULL ? delta : "");
  rc = lua_pcall(ctx->L, 1, 1, 0);
  if (rc != LUA_OK) {
    return CAI_ERR_INVALID;
  }
  lua_pop(ctx->L, 1);
  return CAI_OK;
}

static int cai_lua_stream_function_delta(void *context, const char *item_id,
                                         int output_index, const char *delta,
                                         cai_error *error) {
  cai_lua_function_call_ctx *ctx;
  int rc;
  (void)error;
  ctx = (cai_lua_function_call_ctx *)context;
  if (ctx == NULL || ctx->delta_ref == 0) {
    return CAI_OK;
  }
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->delta_ref);
  lua_pushstring(ctx->L, item_id != NULL ? item_id : "");
  lua_pushinteger(ctx->L, output_index);
  lua_pushstring(ctx->L, delta != NULL ? delta : "");
  rc = lua_pcall(ctx->L, 3, 1, 0);
  if (rc != LUA_OK) {
    lua_pop(ctx->L, 1);
    return CAI_ERR_INVALID;
  }
  if (lua_isboolean(ctx->L, -1) && !lua_toboolean(ctx->L, -1)) {
    lua_pop(ctx->L, 1);
    return CAI_ERR_CANCELLED;
  }
  lua_pop(ctx->L, 1);
  return CAI_OK;
}

static int cai_lua_stream_function_done(void *context, const char *item_id,
                                        int output_index, const char *call_id,
                                        const char *name, const char *arguments,
                                        cai_error *error) {
  cai_lua_function_call_ctx *ctx;
  int rc;
  (void)error;
  ctx = (cai_lua_function_call_ctx *)context;
  if (ctx == NULL || ctx->done_ref == 0) {
    return CAI_OK;
  }
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->done_ref);
  lua_pushstring(ctx->L, item_id != NULL ? item_id : "");
  lua_pushinteger(ctx->L, output_index);
  lua_pushstring(ctx->L, call_id != NULL ? call_id : "");
  lua_pushstring(ctx->L, name != NULL ? name : "");
  lua_pushstring(ctx->L, arguments != NULL ? arguments : "");
  rc = lua_pcall(ctx->L, 5, 1, 0);
  if (rc != LUA_OK) {
    lua_pop(ctx->L, 1);
    return CAI_ERR_INVALID;
  }
  if (lua_isboolean(ctx->L, -1) && !lua_toboolean(ctx->L, -1)) {
    lua_pop(ctx->L, 1);
    return CAI_ERR_CANCELLED;
  }
  lua_pop(ctx->L, 1);
  return CAI_OK;
}

static void cai_lua_apply_affix(lua_State *L, int index, const char *field,
                                cai_stream_affix *affix) {
  index = lua_absindex(L, index);
  lua_getfield(L, index, field);
  if (lua_type(L, -1) == LUA_TSTRING) {
    affix->text = lua_tostring(L, -1);
  }
  lua_pop(L, 1);
}

static int cai_lua_agent_stream(lua_State *L) {
  cai_lua_agent *self;
  cai_stream_sinks sinks;
  cai_run_options options;
  cai_lua_tool_event_ctx event_ctx;
  cai_sink *reasoning_sink;
  cai_sink *output_sink;
  cai_lua_sink_ctx reasoning_ctx;
  cai_lua_sink_ctx output_ctx;
  cai_lua_function_call_ctx function_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  cai_error_init(&error);
  cai_stream_sinks_init(&sinks);
  memset(&event_ctx, 0, sizeof(event_ctx));
  cai_lua_run_options_from_table(L, 2, &options, &event_ctx);
  memset(&reasoning_ctx, 0, sizeof(reasoning_ctx));
  memset(&output_ctx, 0, sizeof(output_ctx));
  memset(&function_ctx, 0, sizeof(function_ctx));
  function_ctx.L = L;
  reasoning_sink = NULL;
  output_sink = NULL;
  lua_getfield(L, 2, "reasoning");
  if (lua_isfunction(L, -1)) {
    rc = cai_lua_make_sink(L, -1, &reasoning_ctx, &reasoning_sink, &error);
    if (rc != CAI_OK) {
      lua_pop(L, 1);
      return cai_lua_fail(L, rc, &error);
    }
    sinks.reasoning_summary = reasoning_sink;
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "response");
  if (lua_isfunction(L, -1)) {
    rc = cai_lua_make_sink(L, -1, &output_ctx, &output_sink, &error);
    if (rc != CAI_OK) {
      lua_pop(L, 1);
      return cai_lua_fail(L, rc, &error);
    }
    sinks.output_text = output_sink;
  }
  lua_pop(L, 1);
  cai_lua_apply_affix(L, 2, "reasoning_prefix",
                      &sinks.reasoning_summary_prefix);
  cai_lua_apply_affix(L, 2, "reasoning_suffix",
                      &sinks.reasoning_summary_suffix);
  cai_lua_apply_affix(L, 2, "response_prefix", &sinks.output_text_prefix);
  cai_lua_apply_affix(L, 2, "response_suffix", &sinks.output_text_suffix);
  lua_getfield(L, 2, "on_response_delta");
  if (lua_isfunction(L, -1)) {
    if (output_ctx.callback_ref == LUA_NOREF || output_ctx.callback_ref == 0) {
      lua_pushvalue(L, -1);
      output_ctx.L = L;
      output_ctx.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    sinks.output_text_delta = cai_lua_stream_output_delta;
    sinks.output_text_context = &output_ctx;
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "on_function_call_delta");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -1);
    function_ctx.delta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sinks.function_call_arguments_delta = cai_lua_stream_function_delta;
    sinks.function_call_context = &function_ctx;
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "on_function_call_done");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -1);
    function_ctx.done_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sinks.function_call_arguments_done = cai_lua_stream_function_done;
    if (sinks.function_call_context == NULL) {
      sinks.function_call_context = &function_ctx;
    }
  }
  lua_pop(L, 1);
  rc = self->ptr->stream_auto(self->ptr, &options, &sinks, &error);
  if (event_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, event_ctx.callback_ref);
  }
  if (reasoning_sink != NULL) {
    cai_sink_close(reasoning_sink);
  }
  if (output_sink != NULL) {
    cai_sink_close(output_sink);
  }
  if (reasoning_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, reasoning_ctx.callback_ref);
  }
  if (output_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, output_ctx.callback_ref);
  }
  if (function_ctx.delta_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, function_ctx.delta_ref);
  }
  if (function_ctx.done_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, function_ctx.done_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_register_raw_tool(lua_State *L) {
  cai_lua_agent *self;
  cai_error error;
  cai_lua_tool_context *ctx;
  cai_lua_tool_ref *ref;
  const char *name;
  const char *description;
  const char *schema_json;
  int strict;
  int rc;
  self = cai_lua_check_agent(L, 1);
  name = luaL_checkstring(L, 2);
  description = luaL_checkstring(L, 3);
  schema_json = luaL_checkstring(L, 4);
  luaL_checktype(L, 5, LUA_TFUNCTION);
  strict = lua_toboolean(L, 6);
  ctx = (cai_lua_tool_context *)calloc(1u, sizeof(*ctx));
  ref = (cai_lua_tool_ref *)calloc(1u, sizeof(*ref));
  if (ctx == NULL || ref == NULL) {
    free(ctx);
    free(ref);
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  ctx->L = L;
  lua_pushvalue(L, 5);
  ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  cai_error_init(&error);
  rc = cai_agent_register_raw_tool(self->ptr, name, description, schema_json,
                                   strict, cai_lua_raw_tool_trampoline, ctx,
                                   &error);
  if (rc != CAI_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->callback_ref);
    free(ctx);
    free(ref);
    return cai_lua_fail(L, rc, &error);
  }
  ref->callback_ref = ctx->callback_ref;
  ref->context = ctx;
  ref->next = self->tools;
  self->tools = ref;
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}

static int cai_lua_agent_register_raw_spooled_tool(lua_State *L) {
  cai_lua_agent *self;
  cai_error error;
  cai_lua_tool_context *ctx;
  cai_lua_tool_ref *ref;
  const char *name;
  const char *description;
  const char *schema_json;
  int strict;
  int rc;
  self = cai_lua_check_agent(L, 1);
  name = luaL_checkstring(L, 2);
  description = luaL_checkstring(L, 3);
  schema_json = luaL_checkstring(L, 4);
  luaL_checktype(L, 5, LUA_TFUNCTION);
  strict = lua_toboolean(L, 6);
  ctx = (cai_lua_tool_context *)calloc(1u, sizeof(*ctx));
  ref = (cai_lua_tool_ref *)calloc(1u, sizeof(*ref));
  if (ctx == NULL || ref == NULL) {
    free(ctx);
    free(ref);
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  ctx->L = L;
  lua_pushvalue(L, 5);
  ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  cai_error_init(&error);
  rc = cai_agent_register_raw_spooled_tool(
      self->ptr, name, description, schema_json, strict,
      cai_lua_raw_spooled_tool_trampoline, ctx, &error);
  if (rc != CAI_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->callback_ref);
    free(ctx);
    free(ref);
    return cai_lua_fail(L, rc, &error);
  }
  ref->callback_ref = ctx->callback_ref;
  ref->context = ctx;
  ref->next = self->tools;
  self->tools = ref;
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}

static void cai_lua_revgeo_config(lua_State *L, int index,
                                  cai_revgeo_tool_config *config) {
  memset(config, 0, sizeof(*config));
  if (!lua_istable(L, index)) {
    return;
  }
  config->name = cai_lua_opt_string_field(L, index, "name", NULL);
  config->description = cai_lua_opt_string_field(L, index, "description", NULL);
  config->base_url = cai_lua_opt_string_field(L, index, "base_url", NULL);
  config->reverse_path =
      cai_lua_opt_string_field(L, index, "reverse_path", NULL);
  config->user_agent = cai_lua_opt_string_field(L, index, "user_agent", NULL);
  config->language = cai_lua_opt_string_field(L, index, "language", NULL);
  config->zoom = cai_lua_opt_int_field(L, index, "zoom", 0);
  config->timeout_ms = cai_lua_opt_long_field(L, index, "timeout_ms", 0);
  config->response_memory_limit =
      cai_lua_opt_size_field(L, index, "response_memory_limit", 0u);
  config->response_max_bytes =
      cai_lua_opt_size_field(L, index, "response_max_bytes", 0u);
  config->response_spool_dir =
      cai_lua_opt_string_field(L, index, "response_spool_dir", NULL);
}

static void cai_lua_searxng_config(lua_State *L, int index,
                                   cai_searxng_tool_config *config) {
  memset(config, 0, sizeof(*config));
  if (!lua_istable(L, index)) {
    return;
  }
  config->name = cai_lua_opt_string_field(L, index, "name", NULL);
  config->description = cai_lua_opt_string_field(L, index, "description", NULL);
  config->base_url = cai_lua_opt_string_field(L, index, "base_url", NULL);
  config->search_path = cai_lua_opt_string_field(L, index, "search_path", NULL);
  config->engine = cai_lua_opt_string_field(L, index, "engine", NULL);
  config->language = cai_lua_opt_string_field(L, index, "language", NULL);
  config->timeout_ms = cai_lua_opt_long_field(L, index, "timeout_ms", 0);
  config->response_memory_limit =
      cai_lua_opt_size_field(L, index, "response_memory_limit", 0u);
  config->response_max_bytes =
      cai_lua_opt_size_field(L, index, "response_max_bytes", 0u);
  config->response_spool_dir =
      cai_lua_opt_string_field(L, index, "response_spool_dir", NULL);
}

static void cai_lua_todo_config(lua_State *L, int index,
                                cai_todo_tool_config *config) {
  memset(config, 0, sizeof(*config));
  if (!lua_istable(L, index)) {
    return;
  }
  config->name = cai_lua_opt_string_field(L, index, "name", NULL);
  config->description = cai_lua_opt_string_field(L, index, "description", NULL);
  config->store_path = cai_lua_opt_string_field(L, index, "store_path", NULL);
  config->lock_path = cai_lua_opt_string_field(L, index, "lock_path", NULL);
  config->default_board =
      cai_lua_opt_string_field(L, index, "default_board", NULL);
  config->max_title_bytes =
      cai_lua_opt_size_field(L, index, "max_title_bytes", 0u);
  config->max_description_bytes =
      cai_lua_opt_size_field(L, index, "max_description_bytes", 0u);
  config->max_result_items =
      cai_lua_opt_size_field(L, index, "max_result_items", 0u);
}

static int cai_lua_agent_register_revgeo(lua_State *L) {
  cai_lua_agent *self;
  cai_revgeo_tool_config config;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_lua_revgeo_config(L, 2, &config);
  cai_error_init(&error);
  rc = cai_agent_register_revgeo_tool(self->ptr, &config, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_register_searxng(lua_State *L) {
  cai_lua_agent *self;
  cai_searxng_tool_config config;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_lua_searxng_config(L, 2, &config);
  cai_error_init(&error);
  rc = cai_agent_register_searxng_tool(self->ptr, &config, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_agent_register_todo(lua_State *L) {
  cai_lua_agent *self;
  cai_todo_tool_config config;
  cai_error error;
  int rc;
  self = cai_lua_check_agent(L, 1);
  cai_lua_todo_config(L, 2, &config);
  cai_error_init(&error);
  rc = cai_agent_register_todo_tool(self->ptr, &config, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_gc(lua_State *L) {
  cai_lua_session *self;
  self = (cai_lua_session *)luaL_checkudata(L, 1, CAI_LUA_SESSION);
  if (self->ptr != NULL) {
    cai_session_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_session_close(lua_State *L) { return cai_lua_session_gc(L); }

static int cai_lua_session_add_user_text(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_add_user_text(self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_add_user_text_source(lua_State *L) {
  cai_lua_session *self;
  cai_source *source;
  cai_lua_source_ctx source_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  source = NULL;
  rc = cai_lua_make_source(L, 2, &source_ctx, &source, &error);
  if (rc == CAI_OK) {
    rc = cai_session_add_user_text_source(self->ptr, source, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  if (source_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_add_user_text_spooled(lua_State *L) {
  cai_lua_session *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 2, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_session_add_user_text_spooled(self->ptr, &spool, &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_add_user_image_url(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_add_user_image_url(self->ptr, luaL_checkstring(L, 2),
                                      luaL_optstring(L, 3, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_add_user_file_path(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_add_user_file_path(self->ptr, luaL_checkstring(L, 2),
                                      luaL_optstring(L, 3, NULL),
                                      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_add_user_file_data_spooled(lua_State *L) {
  cai_lua_session *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 3, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_session_add_user_file_data_spooled(
        self->ptr, luaL_checkstring(L, 2), &spool, luaL_optstring(L, 4, NULL),
        &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_add_function_call_output(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_add_function_call_output(self->ptr, luaL_checkstring(L, 2),
                                            luaL_checkstring(L, 3), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_set_previous_response_id(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_set_previous_response_id(self->ptr, luaL_checkstring(L, 2),
                                            &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_set_conversation_id(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_set_conversation_id(self->ptr, luaL_checkstring(L, 2),
                                       &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_set_conversation(lua_State *L) {
  cai_lua_session *self;
  cai_lua_conversation *conversation;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  conversation = cai_lua_check_conversation(L, 2);
  cai_error_init(&error);
  rc = cai_session_set_conversation(self->ptr, conversation->ptr, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_run(lua_State *L) {
  cai_lua_session *self;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_run(self->ptr, &response, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_run_auto(lua_State *L) {
  cai_lua_session *self;
  cai_response *response;
  cai_run_options options;
  cai_lua_tool_event_ctx event_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  memset(&event_ctx, 0, sizeof(event_ctx));
  cai_error_init(&error);
  cai_lua_run_options_from_table(L, 2, &options, &event_ctx);
  rc = cai_session_run_auto(self->ptr, &options, &response, &error);
  if (event_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, event_ctx.callback_ref);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_run_output(lua_State *L) {
  cai_lua_session *self;
  cai_output *output;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_run_output(self->ptr, &output, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_output(L, output);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_run_auto_output(lua_State *L) {
  cai_lua_session *self;
  cai_output *output;
  cai_run_options options;
  cai_lua_tool_event_ctx event_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  memset(&event_ctx, 0, sizeof(event_ctx));
  cai_error_init(&error);
  cai_lua_run_options_from_table(L, 2, &options, &event_ctx);
  rc = cai_session_run_auto_output(self->ptr, &options, &output, &error);
  if (event_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, event_ctx.callback_ref);
  }
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_output(L, output);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_stream_text(lua_State *L) {
  cai_lua_session *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  sink = NULL;
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_session_stream_text(self->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_stream(lua_State *L) {
  cai_lua_session *self;
  cai_stream_sinks sinks;
  cai_run_options options;
  cai_lua_tool_event_ctx event_ctx;
  cai_sink *reasoning_sink;
  cai_sink *output_sink;
  cai_lua_sink_ctx reasoning_ctx;
  cai_lua_sink_ctx output_ctx;
  cai_lua_function_call_ctx function_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  cai_error_init(&error);
  cai_stream_sinks_init(&sinks);
  memset(&event_ctx, 0, sizeof(event_ctx));
  cai_lua_run_options_from_table(L, 2, &options, &event_ctx);
  memset(&reasoning_ctx, 0, sizeof(reasoning_ctx));
  memset(&output_ctx, 0, sizeof(output_ctx));
  memset(&function_ctx, 0, sizeof(function_ctx));
  function_ctx.L = L;
  reasoning_sink = NULL;
  output_sink = NULL;
  lua_getfield(L, 2, "reasoning");
  if (lua_isfunction(L, -1)) {
    rc = cai_lua_make_sink(L, -1, &reasoning_ctx, &reasoning_sink, &error);
    if (rc != CAI_OK) {
      lua_pop(L, 1);
      return cai_lua_fail(L, rc, &error);
    }
    sinks.reasoning_summary = reasoning_sink;
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "response");
  if (lua_isfunction(L, -1)) {
    rc = cai_lua_make_sink(L, -1, &output_ctx, &output_sink, &error);
    if (rc != CAI_OK) {
      lua_pop(L, 1);
      return cai_lua_fail(L, rc, &error);
    }
    sinks.output_text = output_sink;
  }
  lua_pop(L, 1);
  cai_lua_apply_affix(L, 2, "reasoning_prefix",
                      &sinks.reasoning_summary_prefix);
  cai_lua_apply_affix(L, 2, "reasoning_suffix",
                      &sinks.reasoning_summary_suffix);
  cai_lua_apply_affix(L, 2, "response_prefix", &sinks.output_text_prefix);
  cai_lua_apply_affix(L, 2, "response_suffix", &sinks.output_text_suffix);
  lua_getfield(L, 2, "on_function_call_delta");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -1);
    function_ctx.delta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sinks.function_call_arguments_delta = cai_lua_stream_function_delta;
    sinks.function_call_context = &function_ctx;
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "on_function_call_done");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -1);
    function_ctx.done_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sinks.function_call_arguments_done = cai_lua_stream_function_done;
    if (sinks.function_call_context == NULL) {
      sinks.function_call_context = &function_ctx;
    }
  }
  lua_pop(L, 1);
  rc = cai_session_stream_auto(self->ptr, &options, &sinks, &error);
  if (event_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, event_ctx.callback_ref);
  }
  if (reasoning_sink != NULL) {
    cai_sink_close(reasoning_sink);
  }
  if (output_sink != NULL) {
    cai_sink_close(output_sink);
  }
  if (reasoning_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, reasoning_ctx.callback_ref);
  }
  if (output_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, output_ctx.callback_ref);
  }
  if (function_ctx.delta_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, function_ctx.delta_ref);
  }
  if (function_ctx.done_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, function_ctx.done_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_send_text(lua_State *L) {
  cai_lua_session *self;
  cai_response *response;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_send_text(self->ptr, luaL_checkstring(L, 2), &response,
                             &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_response(L, response);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_last_usage(lua_State *L) {
  cai_lua_session *self;
  cai_token_usage usage;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_last_usage(self->ptr, &usage, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_usage(L, &usage);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_save_state_path(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_save_state_path(self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_export_state(lua_State *L) {
  cai_lua_session *self;
  cai_source *source;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  source = NULL;
  cai_error_init(&error);
  rc = cai_session_export_state_source(self->ptr, &source, &error);
  if (rc == CAI_OK) {
    rc = cai_lua_copy_source_to_callback(L, source, 2, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_import_state(lua_State *L) {
  cai_lua_session *self;
  cai_source *source;
  cai_lua_source_ctx source_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  source = NULL;
  cai_error_init(&error);
  rc = cai_lua_make_source(L, 2, &source_ctx, &source, &error);
  if (rc == CAI_OK) {
    rc = cai_session_import_state_source(self->ptr, source, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  if (source_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_export_history(lua_State *L) {
  cai_lua_session *self;
  cai_source *source;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  source = NULL;
  cai_error_init(&error);
  rc = cai_session_export_history_source(self->ptr, &source, &error);
  if (rc == CAI_OK) {
    rc = cai_lua_copy_source_to_callback(L, source, 2, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_import_history(lua_State *L) {
  cai_lua_session *self;
  cai_source *source;
  cai_lua_source_ctx source_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  source = NULL;
  cai_error_init(&error);
  rc = cai_lua_make_source(L, 2, &source_ctx, &source, &error);
  if (rc == CAI_OK) {
    rc = cai_session_import_history_source(self->ptr, source, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  if (source_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_context(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  double percent;
  int rc;
  self = cai_lua_check_session(L, 1);
  percent = 0.0;
  cai_error_init(&error);
  rc = cai_session_context_percent(self->ptr, &percent, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)cai_session_context_window_tokens(self->ptr));
  lua_setfield(L, -2, "context_window_tokens");
  lua_pushinteger(L,
                  (lua_Integer)cai_session_auto_compact_token_limit(self->ptr));
  lua_setfield(L, -2, "auto_compact_token_limit");
  lua_pushnumber(L, percent);
  lua_setfield(L, -2, "percent");
  lua_pushboolean(L, cai_session_history_spilled(self->ptr));
  lua_setfield(L, -2, "history_spilled");
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_session_load_state_path(lua_State *L) {
  cai_lua_session *self;
  cai_error error;
  int rc;
  self = cai_lua_check_session(L, 1);
  cai_error_init(&error);
  rc = cai_session_load_state_path(self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_session_ids(lua_State *L) {
  cai_lua_session *self;
  self = cai_lua_check_session(L, 1);
  lua_newtable(L);
  lua_pushstring(L, cai_session_previous_response_id(self->ptr));
  lua_setfield(L, -2, "previous_response_id");
  lua_pushstring(L, cai_session_conversation_id(self->ptr));
  lua_setfield(L, -2, "conversation_id");
  return 1;
}

static int cai_lua_response_gc(lua_State *L) {
  cai_lua_response *self;
  self = (cai_lua_response *)luaL_checkudata(L, 1, CAI_LUA_RESPONSE);
  if (self->ptr != NULL) {
    cai_response_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_response_close(lua_State *L) {
  return cai_lua_response_gc(L);
}

static int cai_lua_response_output_text(lua_State *L) {
  cai_lua_response *self;
  const char *value;
  self = cai_lua_check_response(L, 1);
  value = cai_response_output_text(self->ptr);
  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }
  return 1;
}

static int cai_lua_response_raw_json(lua_State *L) {
  cai_lua_response *self;
  const char *value;
  self = cai_lua_check_response(L, 1);
  value = cai_response_raw_json(self->ptr);
  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }
  return 1;
}

static int cai_lua_response_id(lua_State *L) {
  cai_lua_response *self;
  const char *value;
  self = cai_lua_check_response(L, 1);
  value = cai_response_id(self->ptr);
  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }
  return 1;
}

static int cai_lua_response_status(lua_State *L) {
  cai_lua_response *self;
  self = cai_lua_check_response(L, 1);
  lua_pushstring(L, cai_response_status(self->ptr));
  return 1;
}

static int cai_lua_response_model(lua_State *L) {
  cai_lua_response *self;
  self = cai_lua_check_response(L, 1);
  lua_pushstring(L, cai_response_model(self->ptr));
  return 1;
}

static int cai_lua_response_refusal(lua_State *L) {
  cai_lua_response *self;
  self = cai_lua_check_response(L, 1);
  lua_pushstring(L, cai_response_refusal(self->ptr));
  return 1;
}

static int cai_lua_response_write_output_text(lua_State *L) {
  cai_lua_response *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_response(L, 1);
  sink = NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_response_write_output_text(self->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_response_write_refusal(lua_State *L) {
  cai_lua_response *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_response(L, 1);
  sink = NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_response_write_refusal(self->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_response_tool_calls(lua_State *L) {
  cai_lua_response *self;
  size_t i;
  size_t count;
  self = cai_lua_check_response(L, 1);
  count = cai_response_tool_call_count(self->ptr);
  lua_newtable(L);
  for (i = 0u; i < count; i++) {
    lua_newtable(L);
    lua_pushstring(L, cai_response_tool_call_id(self->ptr, i));
    lua_setfield(L, -2, "id");
    lua_pushstring(L, cai_response_tool_call_name(self->ptr, i));
    lua_setfield(L, -2, "name");
    lua_pushstring(L, cai_response_tool_call_arguments(self->ptr, i));
    lua_setfield(L, -2, "arguments");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int cai_lua_response_output_items(lua_State *L) {
  cai_lua_response *self;
  size_t i;
  size_t count;
  self = cai_lua_check_response(L, 1);
  count = cai_response_output_item_count(self->ptr);
  lua_newtable(L);
  for (i = 0u; i < count; i++) {
    lua_newtable(L);
    lua_pushstring(L, cai_response_output_item_id(self->ptr, i));
    lua_setfield(L, -2, "id");
    lua_pushstring(L, cai_response_output_item_type(self->ptr, i));
    lua_setfield(L, -2, "type");
    lua_pushstring(L, cai_response_output_item_status(self->ptr, i));
    lua_setfield(L, -2, "status");
    lua_pushstring(L, cai_response_output_item_role(self->ptr, i));
    lua_setfield(L, -2, "role");
    lua_pushstring(L, cai_response_output_item_call_id(self->ptr, i));
    lua_setfield(L, -2, "call_id");
    lua_pushstring(L, cai_response_output_item_name(self->ptr, i));
    lua_setfield(L, -2, "name");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int cai_lua_response_usage(lua_State *L) {
  cai_lua_response *self;
  cai_token_usage usage;
  cai_error error;
  int rc;
  self = cai_lua_check_response(L, 1);
  cai_error_init(&error);
  rc = cai_response_usage(self->ptr, &usage, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_usage(L, &usage);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_response_summary(lua_State *L) {
  cai_lua_response *self;
  self = cai_lua_check_response(L, 1);
  lua_newtable(L);
  lua_pushstring(L, cai_response_id(self->ptr));
  lua_setfield(L, -2, "id");
  lua_pushstring(L, cai_response_status(self->ptr));
  lua_setfield(L, -2, "status");
  lua_pushstring(L, cai_response_model(self->ptr));
  lua_setfield(L, -2, "model");
  lua_pushstring(L, cai_response_conversation_id(self->ptr));
  lua_setfield(L, -2, "conversation_id");
  lua_pushinteger(L, (lua_Integer)cai_response_created_at(self->ptr));
  lua_setfield(L, -2, "created_at");
  lua_pushstring(L, cai_response_error_code(self->ptr));
  lua_setfield(L, -2, "error_code");
  lua_pushstring(L, cai_response_error_message(self->ptr));
  lua_setfield(L, -2, "error_message");
  lua_pushstring(L, cai_response_incomplete_reason(self->ptr));
  lua_setfield(L, -2, "incomplete_reason");
  lua_pushinteger(L, (lua_Integer)cai_response_tool_call_count(self->ptr));
  lua_setfield(L, -2, "tool_call_count");
  lua_pushinteger(L, (lua_Integer)cai_response_output_item_count(self->ptr));
  lua_setfield(L, -2, "output_item_count");
  return 1;
}

static int cai_lua_output_gc(lua_State *L) {
  cai_lua_output *self;
  self = (cai_lua_output *)luaL_checkudata(L, 1, CAI_LUA_OUTPUT);
  if (self->ptr != NULL) {
    cai_output_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_output_close(lua_State *L) { return cai_lua_output_gc(L); }

static int cai_lua_output_text(lua_State *L) {
  cai_lua_output *self;
  const char *value;
  self = cai_lua_check_output(L, 1);
  value = cai_output_text(self->ptr);
  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }
  return 1;
}

static int cai_lua_output_refusal(lua_State *L) {
  cai_lua_output *self;
  const char *value;
  self = cai_lua_check_output(L, 1);
  value = cai_output_refusal(self->ptr);
  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }
  return 1;
}

static int cai_lua_output_raw_json(lua_State *L) {
  cai_lua_output *self;
  const char *value;
  self = cai_lua_check_output(L, 1);
  value = cai_output_raw_json(self->ptr);
  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }
  return 1;
}

static int cai_lua_output_write_text(lua_State *L) {
  cai_lua_output *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_output(L, 1);
  sink = NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_output_write_text(self->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_output_write_refusal(lua_State *L) {
  cai_lua_output *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_output(L, 1);
  sink = NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_output_write_refusal(self->ptr, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_spool_reader_gc(lua_State *L) {
  cai_lua_spool_reader *self;
  self = (cai_lua_spool_reader *)luaL_checkudata(L, 1, CAI_LUA_SPOOL_READER);
  if (self->owns_cursor) {
    lonejson_spooled_cleanup(&self->cursor);
  }
  memset(self, 0, sizeof(*self));
  return 0;
}

static int cai_lua_spool_reader_size(lua_State *L) {
  cai_lua_spool_reader *self;
  self = cai_lua_check_spool_reader(L, 1);
  lua_pushinteger(L, (lua_Integer)lonejson_spooled_size(&self->cursor));
  return 1;
}

static int cai_lua_spool_reader_spilled(lua_State *L) {
  cai_lua_spool_reader *self;
  self = cai_lua_check_spool_reader(L, 1);
  lua_pushboolean(L, lonejson_spooled_spilled(&self->cursor));
  return 1;
}

static int cai_lua_spool_reader_rewind(lua_State *L) {
  cai_lua_spool_reader *self;
  lonejson_error json_error;
  self = cai_lua_check_spool_reader(L, 1);
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&self->cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lua_pushnil(L);
    lua_pushstring(L, json_error.message);
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int cai_lua_spool_reader_read(lua_State *L) {
  cai_lua_spool_reader *self;
  lonejson_read_result result;
  unsigned char *buffer;
  size_t capacity;
  self = cai_lua_check_spool_reader(L, 1);
  capacity = (size_t)luaL_optinteger(L, 2, 4096);
  if (capacity == 0U) {
    capacity = 4096U;
  }
  buffer = (unsigned char *)malloc(capacity);
  if (buffer == NULL) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  result = lonejson_spooled_read(&self->cursor, buffer, capacity);
  if (result.error_code != 0) {
    free(buffer);
    lua_pushnil(L);
    lua_pushstring(L, "failed to read spooled value");
    return 2;
  }
  if (result.eof && result.bytes_read == 0U) {
    free(buffer);
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, (const char *)buffer, result.bytes_read);
  free(buffer);
  return 1;
}

static int cai_lua_spool_reader_read_all(lua_State *L) {
  luaL_Buffer buffer;
  luaL_buffinit(L, &buffer);
  lua_pushcfunction(L, cai_lua_spool_reader_rewind);
  lua_pushvalue(L, 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK || lua_isnil(L, -1)) {
    return luaL_error(L, "failed to rewind spooled value");
  }
  lua_pop(L, 1);
  for (;;) {
    lua_pushcfunction(L, cai_lua_spool_reader_read);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 4096);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
      return lua_error(L);
    }
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    luaL_addvalue(&buffer);
  }
  luaL_pushresult(&buffer);
  return 1;
}

static int cai_lua_spool_reader_write(lua_State *L) {
  cai_lua_spool_reader *self;
  cai_sink *sink;
  cai_lua_sink_ctx sink_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_spool_reader(L, 1);
  sink = NULL;
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  cai_error_init(&error);
  rc = cai_lua_make_sink(L, 2, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_lua_stream_lua_reader_to_sink(L, 1, sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  (void)self;
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_registry_gc(lua_State *L) {
  cai_lua_registry *self;
  self = (cai_lua_registry *)luaL_checkudata(L, 1, CAI_LUA_REGISTRY);
  if (self->ptr != NULL) {
    cai_tool_registry_destroy(self->ptr);
    self->ptr = NULL;
  }
  cai_lua_unref_tools(L, self->tools);
  self->tools = NULL;
  return 0;
}

static int cai_lua_registry_close(lua_State *L) {
  return cai_lua_registry_gc(L);
}

static int cai_lua_registry_new(lua_State *L) {
  cai_tool_registry *registry;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_tool_registry_new(&registry, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_registry(L, registry);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_registry_register_revgeo(lua_State *L) {
  cai_lua_registry *self;
  cai_revgeo_tool_config config;
  cai_error error;
  int rc;
  self = cai_lua_check_registry(L, 1);
  cai_lua_revgeo_config(L, 2, &config);
  cai_error_init(&error);
  rc = cai_tool_registry_register_revgeo_tool(self->ptr, &config, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_registry_register_searxng(lua_State *L) {
  cai_lua_registry *self;
  cai_searxng_tool_config config;
  cai_error error;
  int rc;
  self = cai_lua_check_registry(L, 1);
  cai_lua_searxng_config(L, 2, &config);
  cai_error_init(&error);
  rc = cai_tool_registry_register_searxng_tool(self->ptr, &config, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_registry_register_todo(lua_State *L) {
  cai_lua_registry *self;
  cai_todo_tool_config config;
  cai_error error;
  int rc;
  self = cai_lua_check_registry(L, 1);
  cai_lua_todo_config(L, 2, &config);
  cai_error_init(&error);
  rc = cai_tool_registry_register_todo_tool(self->ptr, &config, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_registry_register_raw_tool(lua_State *L) {
  cai_lua_registry *self;
  cai_error error;
  cai_lua_tool_context *ctx;
  cai_lua_tool_ref *ref;
  const char *name;
  const char *description;
  const char *schema_json;
  int strict;
  int rc;
  self = cai_lua_check_registry(L, 1);
  name = luaL_checkstring(L, 2);
  description = luaL_checkstring(L, 3);
  schema_json = luaL_checkstring(L, 4);
  luaL_checktype(L, 5, LUA_TFUNCTION);
  strict = lua_toboolean(L, 6);
  ctx = (cai_lua_tool_context *)calloc(1u, sizeof(*ctx));
  ref = (cai_lua_tool_ref *)calloc(1u, sizeof(*ref));
  if (ctx == NULL || ref == NULL) {
    free(ctx);
    free(ref);
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  ctx->L = L;
  lua_pushvalue(L, 5);
  ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  cai_error_init(&error);
  rc = cai_tool_registry_register_raw(self->ptr, name, description, schema_json,
                                      strict, cai_lua_raw_tool_trampoline, ctx,
                                      &error);
  if (rc != CAI_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->callback_ref);
    free(ctx);
    free(ref);
    return cai_lua_fail(L, rc, &error);
  }
  ref->callback_ref = ctx->callback_ref;
  ref->context = ctx;
  ref->next = self->tools;
  self->tools = ref;
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}

static int cai_lua_registry_register_raw_spooled_tool(lua_State *L) {
  cai_lua_registry *self;
  cai_error error;
  cai_lua_tool_context *ctx;
  cai_lua_tool_ref *ref;
  const char *name;
  const char *description;
  const char *schema_json;
  int strict;
  int rc;
  self = cai_lua_check_registry(L, 1);
  name = luaL_checkstring(L, 2);
  description = luaL_checkstring(L, 3);
  schema_json = luaL_checkstring(L, 4);
  luaL_checktype(L, 5, LUA_TFUNCTION);
  strict = lua_toboolean(L, 6);
  ctx = (cai_lua_tool_context *)calloc(1u, sizeof(*ctx));
  ref = (cai_lua_tool_ref *)calloc(1u, sizeof(*ref));
  if (ctx == NULL || ref == NULL) {
    free(ctx);
    free(ref);
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  ctx->L = L;
  lua_pushvalue(L, 5);
  ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  cai_error_init(&error);
  rc = cai_tool_registry_register_raw_spooled(
      self->ptr, name, description, schema_json, strict,
      cai_lua_raw_spooled_tool_trampoline, ctx, &error);
  if (rc != CAI_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->callback_ref);
    free(ctx);
    free(ref);
    return cai_lua_fail(L, rc, &error);
  }
  ref->callback_ref = ctx->callback_ref;
  ref->context = ctx;
  ref->next = self->tools;
  self->tools = ref;
  cai_lua_error_cleanup(&error);
  lua_pushboolean(L, 1);
  return 1;
}

static int cai_lua_registry_run(lua_State *L) {
  cai_lua_registry *self;
  cai_lua_sink_ctx sink_ctx;
  cai_sink *sink;
  cai_error error;
  int rc;
  self = cai_lua_check_registry(L, 1);
  cai_error_init(&error);
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  sink = NULL;
  rc = cai_lua_make_sink(L, 4, &sink_ctx, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_tool_registry_run(self->ptr, luaL_checkstring(L, 2),
                               luaL_checkstring(L, 3), sink, &error);
  }
  if (sink != NULL) {
    cai_sink_close(sink);
  }
  luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  return cai_lua_bool_result(L, rc, &error);
}

static const char *cai_lua_mcp_header_get(void *context, const char *name) {
  cai_lua_headers_ctx *ctx;
  const char *value;
  ctx = (cai_lua_headers_ctx *)context;
  value = NULL;
  if (ctx->get_ref != LUA_NOREF && ctx->get_ref != 0) {
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->get_ref);
    lua_pushstring(ctx->L, name);
    if (lua_pcall(ctx->L, 1, 1, 0) == LUA_OK) {
      value = lua_tostring(ctx->L, -1);
    }
    lua_pop(ctx->L, 1);
    return value;
  }
  if (ctx->request_table_ref != LUA_NOREF && ctx->request_table_ref != 0) {
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->request_table_ref);
    lua_getfield(ctx->L, -1, name);
    value = lua_tostring(ctx->L, -1);
    lua_pop(ctx->L, 2);
  }
  return value;
}

static int cai_lua_mcp_header_set(void *context, const char *name,
                                  const char *value, cai_error *error) {
  cai_lua_headers_ctx *ctx;
  (void)error;
  ctx = (cai_lua_headers_ctx *)context;
  if (ctx->set_ref != LUA_NOREF && ctx->set_ref != 0) {
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->set_ref);
    lua_pushstring(ctx->L, name);
    lua_pushstring(ctx->L, value);
    if (lua_pcall(ctx->L, 2, 0, 0) != LUA_OK) {
      lua_pop(ctx->L, 1);
      return CAI_ERR_INVALID;
    }
    return CAI_OK;
  }
  if (ctx->response_table_ref != LUA_NOREF && ctx->response_table_ref != 0) {
    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->response_table_ref);
    lua_pushstring(ctx->L, value);
    lua_setfield(ctx->L, -2, name);
    lua_pop(ctx->L, 1);
  }
  return CAI_OK;
}

static int cai_lua_mcp_new(lua_State *L) {
  cai_mcp_handler_config config;
  cai_lua_registry *registry;
  cai_mcp_handler *handler;
  cai_error error;
  int rc;
  luaL_checktype(L, 1, LUA_TTABLE);
  cai_mcp_handler_config_init(&config);
  config.name = cai_lua_opt_string_field(L, 1, "name", config.name);
  config.version = cai_lua_opt_string_field(L, 1, "version", config.version);
  config.request_max_bytes = cai_lua_opt_size_field(L, 1, "request_max_bytes",
                                                    config.request_max_bytes);
  config.response_spool_memory_limit = cai_lua_opt_size_field(
      L, 1, "response_spool_memory_limit", config.response_spool_memory_limit);
  config.tool_output_max_bytes = cai_lua_opt_size_field(
      L, 1, "tool_output_max_bytes", config.tool_output_max_bytes);
  config.stateless = cai_lua_opt_int_field(L, 1, "stateless", config.stateless);
  config.validate_origin =
      cai_lua_opt_int_field(L, 1, "validate_origin", config.validate_origin);
  config.protocol_version = cai_lua_opt_string_field(L, 1, "protocol_version",
                                                     config.protocol_version);
  config.allow_legacy_no_version = cai_lua_opt_int_field(
      L, 1, "allow_legacy_no_version", config.allow_legacy_no_version);
  lua_getfield(L, 1, "tools");
  registry = cai_lua_check_registry(L, -1);
  config.tools = registry->ptr;
  cai_error_init(&error);
  rc = cai_mcp_handler_new(&config, &handler, &error);
  lua_pop(L, 1);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_mcp(L, handler);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_mcp_gc(lua_State *L) {
  cai_lua_mcp *self;
  self = (cai_lua_mcp *)luaL_checkudata(L, 1, CAI_LUA_MCP);
  if (self->ptr != NULL) {
    cai_mcp_handler_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_mcp_close(lua_State *L) { return cai_lua_mcp_gc(L); }

static int cai_lua_mcp_handle_http(lua_State *L) {
  cai_lua_mcp *self;
  cai_mcp_http_request request;
  cai_mcp_http_response response;
  cai_lua_source_ctx source_ctx;
  cai_lua_sink_ctx sink_ctx;
  cai_lua_headers_ctx headers_ctx;
  cai_source *source;
  cai_sink *sink;
  cai_error error;
  int rc;
  self = cai_lua_check_mcp(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  memset(&headers_ctx, 0, sizeof(headers_ctx));
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  source = NULL;
  sink = NULL;
  headers_ctx.L = L;
  headers_ctx.get_ref = LUA_NOREF;
  headers_ctx.set_ref = LUA_NOREF;
  headers_ctx.request_table_ref = LUA_NOREF;
  headers_ctx.response_table_ref = LUA_NOREF;
  cai_error_init(&error);
  request.method = cai_lua_opt_string_field(L, 2, "method", "POST");
  lua_getfield(L, 2, "body");
  rc = cai_lua_make_source(L, -1, &source_ctx, &source, &error);
  lua_pop(L, 1);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  lua_getfield(L, 2, "headers");
  if (lua_istable(L, -1)) {
    lua_pushvalue(L, -1);
    headers_ctx.request_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_pop(L, 1);
  lua_getfield(L, 2, "header");
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, -1);
    headers_ctx.get_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_pop(L, 1);
  lua_newtable(L);
  headers_ctx.response_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  request.body = source;
  request.header = cai_lua_mcp_header_get;
  request.header_context = &headers_ctx;
  response.set_header = cai_lua_mcp_header_set;
  response.header_context = &headers_ctx;
  lua_getfield(L, 2, "write");
  if (lua_isfunction(L, -1)) {
    rc = cai_lua_make_sink(L, -1, &sink_ctx, &sink, &error);
  }
  lua_pop(L, 1);
  if (rc != CAI_OK) {
    cai_source_close(source);
    return cai_lua_fail(L, rc, &error);
  }
  if (sink == NULL) {
    cai_source_close(source);
    if (source_ctx.callback_ref != 0) {
      luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
    }
    if (headers_ctx.get_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.get_ref);
    }
    if (headers_ctx.set_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.set_ref);
    }
    if (headers_ctx.request_table_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.request_table_ref);
    }
    if (headers_ctx.response_table_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.response_table_ref);
    }
    luaL_error(L, "mcp handle_http requires a streaming write callback");
  }
  response.body = sink;
  rc = cai_mcp_handler_handle_http(self->ptr, &request, &response, &error);
  cai_sink_close(sink);
  cai_source_close(source);
  if (sink_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, sink_ctx.callback_ref);
  }
  if (source_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
  }
  if (headers_ctx.get_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.get_ref);
  }
  if (headers_ctx.set_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.set_ref);
  }
  if (headers_ctx.request_table_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.request_table_ref);
  }
  if (rc != CAI_OK) {
    luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.response_table_ref);
    return cai_lua_fail(L, rc, &error);
  }
  lua_newtable(L);
  lua_pushinteger(L, response.status);
  lua_setfield(L, -2, "status");
  lua_rawgeti(L, LUA_REGISTRYINDEX, headers_ctx.response_table_ref);
  lua_setfield(L, -2, "headers");
  luaL_unref(L, LUA_REGISTRYINDEX, headers_ctx.response_table_ref);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_schema_new(lua_State *L) {
  cai_tool_schema *schema;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_tool_schema_new(&schema, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_schema(L, schema);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_schema_gc(lua_State *L) {
  cai_lua_schema *self;
  self = (cai_lua_schema *)luaL_checkudata(L, 1, CAI_LUA_SCHEMA);
  if (self->ptr != NULL) {
    cai_tool_schema_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_schema_close(lua_State *L) { return cai_lua_schema_gc(L); }

static int cai_lua_schema_json(lua_State *L) {
  cai_lua_schema *self;
  self = cai_lua_check_schema(L, 1);
  lua_pushstring(L, cai_tool_schema_json(self->ptr));
  return 1;
}

static int cai_lua_schema_add_string(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_add_string(self->ptr, luaL_checkstring(L, 2),
                                  luaL_optstring(L, 3, NULL),
                                  lua_toboolean(L, 4), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_add_integer(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_add_integer(self->ptr, luaL_checkstring(L, 2),
                                   luaL_optstring(L, 3, NULL),
                                   lua_toboolean(L, 4), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_add_number(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_add_number(self->ptr, luaL_checkstring(L, 2),
                                  luaL_optstring(L, 3, NULL),
                                  lua_toboolean(L, 4), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_add_boolean(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_add_boolean(self->ptr, luaL_checkstring(L, 2),
                                   luaL_optstring(L, 3, NULL),
                                   lua_toboolean(L, 4), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_set_strict(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_set_strict(self->ptr, lua_toboolean(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_add_string_enum(lua_State *L) {
  cai_lua_schema *self;
  const char **values;
  size_t count;
  size_t i;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  luaL_checktype(L, 4, LUA_TTABLE);
  count = lua_rawlen(L, 4);
  values = (const char **)calloc(count == 0u ? 1u : count, sizeof(*values));
  if (values == NULL) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  for (i = 0u; i < count; i++) {
    lua_rawgeti(L, 4, (lua_Integer)i + 1);
    values[i] = luaL_checkstring(L, -1);
    lua_pop(L, 1);
  }
  cai_error_init(&error);
  rc = cai_tool_schema_add_string_enum(self->ptr, luaL_checkstring(L, 2),
                                       luaL_optstring(L, 3, NULL), values,
                                       count, lua_toboolean(L, 5), &error);
  free(values);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_describe(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_describe(self->ptr, luaL_checkstring(L, 2),
                                luaL_checkstring(L, 3), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_add_raw_property(lua_State *L) {
  cai_lua_schema *self;
  cai_error error;
  int rc;
  self = cai_lua_check_schema(L, 1);
  cai_error_init(&error);
  rc = cai_tool_schema_add_raw_property(
      self->ptr, luaL_checkstring(L, 2), luaL_optstring(L, 3, NULL),
      luaL_checkstring(L, 4), lua_toboolean(L, 5), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_schema_strict(lua_State *L) {
  cai_lua_schema *self;
  self = cai_lua_check_schema(L, 1);
  lua_pushboolean(L, cai_tool_schema_strict(self->ptr));
  return 1;
}

static int cai_lua_model_info(lua_State *L) {
  const cai_model_info *info;
  const char *model;
  model = luaL_checkstring(L, 1);
  info = cai_model_info_by_id(model);
  if (info == NULL) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, info->id);
  lua_setfield(L, -2, "id");
  lua_pushinteger(L, (lua_Integer)info->capabilities);
  lua_setfield(L, -2, "capabilities");
  lua_pushinteger(L, (lua_Integer)info->metadata_flags);
  lua_setfield(L, -2, "metadata_flags");
  lua_pushinteger(L, (lua_Integer)info->context_window_tokens);
  lua_setfield(L, -2, "context_window_tokens");
  lua_pushinteger(L, (lua_Integer)info->auto_compact_token_limit);
  lua_setfield(L, -2, "auto_compact_token_limit");
  lua_pushnumber(L, info->input_usd_per_million);
  lua_setfield(L, -2, "input_usd_per_million");
  lua_pushnumber(L, info->cached_input_usd_per_million);
  lua_setfield(L, -2, "cached_input_usd_per_million");
  lua_pushnumber(L, info->output_usd_per_million);
  lua_setfield(L, -2, "output_usd_per_million");
  return 1;
}

static int cai_lua_conversation_from_id(lua_State *L) {
  cai_conversation *conversation;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_conversation_from_id(luaL_checkstring(L, 1), &conversation, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_conversation(L, conversation);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_conversation_gc(lua_State *L) {
  cai_lua_conversation *self;
  self = (cai_lua_conversation *)luaL_checkudata(L, 1, CAI_LUA_CONVERSATION);
  if (self->ptr != NULL) {
    cai_conversation_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_conversation_close(lua_State *L) {
  return cai_lua_conversation_gc(L);
}

static int cai_lua_conversation_id(lua_State *L) {
  cai_lua_conversation *self;
  self = cai_lua_check_conversation(L, 1);
  lua_pushstring(L, cai_conversation_id(self->ptr));
  return 1;
}

static int cai_lua_conversation_object(lua_State *L) {
  cai_lua_conversation *self;
  self = cai_lua_check_conversation(L, 1);
  lua_pushstring(L, cai_conversation_object(self->ptr));
  return 1;
}

static int cai_lua_input_items_gc(lua_State *L) {
  cai_lua_input_items *self;
  self = (cai_lua_input_items *)luaL_checkudata(L, 1, CAI_LUA_INPUT_ITEMS);
  if (self->ptr != NULL) {
    cai_input_item_list_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_input_items_close(lua_State *L) {
  return cai_lua_input_items_gc(L);
}

static int cai_lua_input_items_summary(lua_State *L) {
  cai_lua_input_items *self;
  size_t i;
  size_t count;
  self = cai_lua_check_input_items(L, 1);
  count = cai_input_item_list_count(self->ptr);
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)count);
  lua_setfield(L, -2, "count");
  lua_pushboolean(L, cai_input_item_list_has_more(self->ptr));
  lua_setfield(L, -2, "has_more");
  lua_pushstring(L, cai_input_item_list_first_id(self->ptr));
  lua_setfield(L, -2, "first_id");
  lua_pushstring(L, cai_input_item_list_last_id(self->ptr));
  lua_setfield(L, -2, "last_id");
  lua_newtable(L);
  for (i = 0u; i < count; i++) {
    lua_newtable(L);
    lua_pushstring(L, cai_input_item_id(self->ptr, i));
    lua_setfield(L, -2, "id");
    lua_pushstring(L, cai_input_item_type(self->ptr, i));
    lua_setfield(L, -2, "type");
    lua_pushstring(L, cai_input_item_role(self->ptr, i));
    lua_setfield(L, -2, "role");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  lua_setfield(L, -2, "items");
  return 1;
}

static int cai_lua_input_items_raw_json(lua_State *L) {
  cai_lua_input_items *self;
  self = cai_lua_check_input_items(L, 1);
  lua_pushstring(L, cai_input_item_list_raw_json(self->ptr));
  return 1;
}

static int cai_lua_conversation_item_gc(lua_State *L) {
  cai_lua_conversation_item *self;
  self = (cai_lua_conversation_item *)luaL_checkudata(
      L, 1, CAI_LUA_CONVERSATION_ITEM);
  if (self->ptr != NULL) {
    cai_conversation_item_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_conversation_item_close(lua_State *L) {
  return cai_lua_conversation_item_gc(L);
}

static int cai_lua_conversation_item_summary(lua_State *L) {
  cai_lua_conversation_item *self;
  self = cai_lua_check_conversation_item(L, 1);
  lua_newtable(L);
  lua_pushstring(L, cai_conversation_item_id(self->ptr));
  lua_setfield(L, -2, "id");
  lua_pushstring(L, cai_conversation_item_type(self->ptr));
  lua_setfield(L, -2, "type");
  lua_pushstring(L, cai_conversation_item_role(self->ptr));
  lua_setfield(L, -2, "role");
  lua_pushstring(L, cai_conversation_item_raw_json(self->ptr));
  lua_setfield(L, -2, "raw_json");
  return 1;
}

static int cai_lua_params_new(lua_State *L) {
  cai_response_create_params *params;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_response_create_params_new(&params, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_params(L, params);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_params_gc(lua_State *L) {
  cai_lua_params *self;
  self = (cai_lua_params *)luaL_checkudata(L, 1, CAI_LUA_PARAMS);
  if (self->ptr != NULL) {
    cai_response_create_params_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_params_close(lua_State *L) { return cai_lua_params_gc(L); }

static int cai_lua_params_set_model(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_model(self->ptr, luaL_checkstring(L, 2),
                                            &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_instructions(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_instructions(
      self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_previous_response_id(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_previous_response_id(
      self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_conversation_id(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_conversation_id(
      self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_prompt_cache_key(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_prompt_cache_key(
      self->ptr, luaL_checkstring(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_tool_choice(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_tool_choice(
      self->ptr, luaL_optstring(L, 2, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_max_output_tokens(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_max_output_tokens(
      self->ptr, (int)luaL_checkinteger(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_parallel_tool_calls(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_parallel_tool_calls(
      self->ptr, lua_toboolean(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_compact_threshold(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_compact_threshold(
      self->ptr, (long long)luaL_checkinteger(L, 2), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_reasoning(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_reasoning(
      self->ptr, luaL_optstring(L, 2, NULL), luaL_optstring(L, 3, NULL),
      &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_text_format_json_object(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc =
      cai_response_create_params_set_text_format_json_object(self->ptr, &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_set_text_format_json_schema(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_set_text_format_json_schema(
      self->ptr, luaL_checkstring(L, 2), luaL_optstring(L, 3, NULL),
      luaL_checkstring(L, 4), lua_toboolean(L, 5), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_text(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_text(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_text_spooled(lua_State *L) {
  cai_lua_params *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 3, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_text_spooled(
        self->ptr, luaL_optstring(L, 2, "user"), &spool, &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_image_url(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_image_url(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_image_file_id(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_image_file_id(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_file_id(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_file_id(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_file_data_spooled(lua_State *L) {
  cai_lua_params *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 4, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_file_data_spooled(
        self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3), &spool,
        luaL_optstring(L, 5, NULL), &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_file_url(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_file_url(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_function_tool(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_function_tool(
      self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3),
      luaL_checkstring(L, 4), lua_toboolean(L, 5), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_function_call_output(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_function_call_output(
      self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_function_call_output_text(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_function_call_output_text(
      self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_function_call_output_image_url(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_function_call_output_image_url(
      self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_params_add_function_call_output_file_id(lua_State *L) {
  cai_lua_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_response_create_params_add_function_call_output_file_id(
      self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int
cai_lua_params_add_function_call_output_file_data_spooled(lua_State *L) {
  cai_lua_params *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_params(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 4, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_function_call_output_file_data_spooled(
        self->ptr, luaL_checkstring(L, 2), luaL_checkstring(L, 3), &spool,
        luaL_optstring(L, 5, NULL), &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_new(lua_State *L) {
  cai_conversation_items_params *params;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_conversation_items_params_new(&params, &error);
  if (rc != CAI_OK) {
    return cai_lua_fail(L, rc, &error);
  }
  cai_lua_push_conversation_params(L, params);
  cai_lua_error_cleanup(&error);
  return 1;
}

static int cai_lua_conversation_params_gc(lua_State *L) {
  cai_lua_conversation_params *self;
  self = (cai_lua_conversation_params *)luaL_checkudata(
      L, 1, CAI_LUA_CONVERSATION_PARAMS);
  if (self->ptr != NULL) {
    cai_conversation_items_params_destroy(self->ptr);
    self->ptr = NULL;
  }
  return 0;
}

static int cai_lua_conversation_params_close(lua_State *L) {
  return cai_lua_conversation_params_gc(L);
}

static int cai_lua_conversation_params_add_text(lua_State *L) {
  cai_lua_conversation_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_conversation_items_params_add_text(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_text_spooled(lua_State *L) {
  cai_lua_conversation_params *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 3, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_conversation_items_params_add_text_spooled(
        self->ptr, luaL_optstring(L, 2, "user"), &spool, &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_text_source(lua_State *L) {
  cai_lua_conversation_params *self;
  cai_source *source;
  cai_lua_source_ctx source_ctx;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  source = NULL;
  cai_error_init(&error);
  rc = cai_lua_make_source(L, 3, &source_ctx, &source, &error);
  if (rc == CAI_OK) {
    rc = cai_conversation_items_params_add_text_source(
        self->ptr, luaL_optstring(L, 2, "user"), source, &error);
  }
  if (source != NULL) {
    cai_source_close(source);
  }
  if (source_ctx.callback_ref != 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, source_ctx.callback_ref);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_file_data_spooled(lua_State *L) {
  cai_lua_conversation_params *self;
  lonejson_spooled spool;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_lua_spool_from_stack(L, 4, &spool, &error);
  if (rc == CAI_OK) {
    rc = cai_conversation_items_params_add_file_data_spooled(
        self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3), &spool,
        luaL_optstring(L, 5, NULL), &error);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&spool);
  }
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_image_url(lua_State *L) {
  cai_lua_conversation_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_conversation_items_params_add_image_url(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_image_file_id(lua_State *L) {
  cai_lua_conversation_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_conversation_items_params_add_image_file_id(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_file_id(lua_State *L) {
  cai_lua_conversation_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_conversation_items_params_add_file_id(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static int cai_lua_conversation_params_add_file_url(lua_State *L) {
  cai_lua_conversation_params *self;
  cai_error error;
  int rc;
  self = cai_lua_check_conversation_params(L, 1);
  cai_error_init(&error);
  rc = cai_conversation_items_params_add_file_url(
      self->ptr, luaL_optstring(L, 2, "user"), luaL_checkstring(L, 3),
      luaL_optstring(L, 4, NULL), &error);
  return cai_lua_bool_result(L, rc, &error);
}

static const luaL_Reg cai_lua_client_methods[] = {
    {"new_agent", cai_lua_client_new_agent},
    {"create_response", cai_lua_client_create_response},
    {"stream_response_text", cai_lua_client_stream_response_text},
    {"retrieve_response", cai_lua_client_retrieve_response},
    {"cancel_response", cai_lua_client_cancel_response},
    {"delete_response", cai_lua_client_delete_response},
    {"list_response_input_items", cai_lua_client_list_response_input_items},
    {"create_conversation", cai_lua_client_create_conversation},
    {"retrieve_conversation", cai_lua_client_retrieve_conversation},
    {"update_conversation_metadata",
     cai_lua_client_update_conversation_metadata},
    {"delete_conversation", cai_lua_client_delete_conversation},
    {"list_conversation_items", cai_lua_client_list_conversation_items},
    {"create_conversation_items", cai_lua_client_create_conversation_items},
    {"retrieve_conversation_item", cai_lua_client_retrieve_conversation_item},
    {"delete_conversation_item", cai_lua_client_delete_conversation_item},
    {"close", cai_lua_client_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_agent_methods[] = {
    {"new_session", cai_lua_agent_new_session},
    {"new_conversation_session", cai_lua_agent_new_conversation_session},
    {"new_session_for_conversation",
     cai_lua_agent_new_session_for_conversation},
    {"add_user_text", cai_lua_agent_add_user_text},
    {"add_user_text_spooled", cai_lua_agent_add_user_text_spooled},
    {"add_user_text_source", cai_lua_agent_add_user_text_source},
    {"add_user_image_url", cai_lua_agent_add_user_image_url},
    {"add_user_file_data_spooled", cai_lua_agent_add_user_file_data_spooled},
    {"add_user_file_path", cai_lua_agent_add_user_file_path},
    {"run", cai_lua_agent_run},
    {"run_auto", cai_lua_agent_run_auto},
    {"run_output", cai_lua_agent_run_output},
    {"send_text", cai_lua_agent_send_text},
    {"stream_text", cai_lua_agent_stream_text},
    {"stream", cai_lua_agent_stream},
    {"last_usage", cai_lua_agent_last_usage},
    {"context_percent", cai_lua_agent_context_percent},
    {"register_raw_tool", cai_lua_agent_register_raw_tool},
    {"register_raw_spooled_tool", cai_lua_agent_register_raw_spooled_tool},
    {"register_revgeo_tool", cai_lua_agent_register_revgeo},
    {"register_searxng_tool", cai_lua_agent_register_searxng},
    {"register_todo_tool", cai_lua_agent_register_todo},
    {"close", cai_lua_agent_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_session_methods[] = {
    {"set_previous_response_id", cai_lua_session_set_previous_response_id},
    {"set_conversation_id", cai_lua_session_set_conversation_id},
    {"set_conversation", cai_lua_session_set_conversation},
    {"add_user_text", cai_lua_session_add_user_text},
    {"add_user_text_spooled", cai_lua_session_add_user_text_spooled},
    {"add_user_text_source", cai_lua_session_add_user_text_source},
    {"add_user_image_url", cai_lua_session_add_user_image_url},
    {"add_user_file_data_spooled", cai_lua_session_add_user_file_data_spooled},
    {"add_user_file_path", cai_lua_session_add_user_file_path},
    {"add_function_call_output", cai_lua_session_add_function_call_output},
    {"run", cai_lua_session_run},
    {"run_auto", cai_lua_session_run_auto},
    {"run_output", cai_lua_session_run_output},
    {"run_auto_output", cai_lua_session_run_auto_output},
    {"send_text", cai_lua_session_send_text},
    {"stream", cai_lua_session_stream},
    {"stream_text", cai_lua_session_stream_text},
    {"last_usage", cai_lua_session_last_usage},
    {"context", cai_lua_session_context},
    {"export_state", cai_lua_session_export_state},
    {"import_state", cai_lua_session_import_state},
    {"export_history", cai_lua_session_export_history},
    {"import_history", cai_lua_session_import_history},
    {"save_state_path", cai_lua_session_save_state_path},
    {"load_state_path", cai_lua_session_load_state_path},
    {"ids", cai_lua_session_ids},
    {"close", cai_lua_session_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_response_methods[] = {
    {"id", cai_lua_response_id},
    {"status", cai_lua_response_status},
    {"model", cai_lua_response_model},
    {"output_text", cai_lua_response_output_text},
    {"refusal", cai_lua_response_refusal},
    {"write_output_text", cai_lua_response_write_output_text},
    {"write_refusal", cai_lua_response_write_refusal},
    {"raw_json", cai_lua_response_raw_json},
    {"usage", cai_lua_response_usage},
    {"tool_calls", cai_lua_response_tool_calls},
    {"output_items", cai_lua_response_output_items},
    {"summary", cai_lua_response_summary},
    {"close", cai_lua_response_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_output_methods[] = {
    {"text", cai_lua_output_text},
    {"refusal", cai_lua_output_refusal},
    {"raw_json", cai_lua_output_raw_json},
    {"write_text", cai_lua_output_write_text},
    {"write_refusal", cai_lua_output_write_refusal},
    {"close", cai_lua_output_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_spool_reader_methods[] = {
    {"size", cai_lua_spool_reader_size},
    {"spilled", cai_lua_spool_reader_spilled},
    {"rewind", cai_lua_spool_reader_rewind},
    {"read", cai_lua_spool_reader_read},
    {"read_all", cai_lua_spool_reader_read_all},
    {"write", cai_lua_spool_reader_write},
    {NULL, NULL}};

static const luaL_Reg cai_lua_registry_methods[] = {
    {"register_raw_tool", cai_lua_registry_register_raw_tool},
    {"register_raw_spooled_tool", cai_lua_registry_register_raw_spooled_tool},
    {"register_revgeo_tool", cai_lua_registry_register_revgeo},
    {"register_searxng_tool", cai_lua_registry_register_searxng},
    {"register_todo_tool", cai_lua_registry_register_todo},
    {"run", cai_lua_registry_run},
    {"close", cai_lua_registry_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_mcp_methods[] = {
    {"handle_http", cai_lua_mcp_handle_http},
    {"close", cai_lua_mcp_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_schema_methods[] = {
    {"set_strict", cai_lua_schema_set_strict},
    {"string", cai_lua_schema_add_string},
    {"integer", cai_lua_schema_add_integer},
    {"number", cai_lua_schema_add_number},
    {"boolean", cai_lua_schema_add_boolean},
    {"string_enum", cai_lua_schema_add_string_enum},
    {"describe", cai_lua_schema_describe},
    {"raw_property", cai_lua_schema_add_raw_property},
    {"json", cai_lua_schema_json},
    {"strict", cai_lua_schema_strict},
    {"close", cai_lua_schema_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_params_methods[] = {
    {"set_model", cai_lua_params_set_model},
    {"set_instructions", cai_lua_params_set_instructions},
    {"set_previous_response_id", cai_lua_params_set_previous_response_id},
    {"set_conversation_id", cai_lua_params_set_conversation_id},
    {"set_prompt_cache_key", cai_lua_params_set_prompt_cache_key},
    {"set_tool_choice", cai_lua_params_set_tool_choice},
    {"set_max_output_tokens", cai_lua_params_set_max_output_tokens},
    {"set_parallel_tool_calls", cai_lua_params_set_parallel_tool_calls},
    {"set_compact_threshold", cai_lua_params_set_compact_threshold},
    {"set_reasoning", cai_lua_params_set_reasoning},
    {"set_text_format_json_object", cai_lua_params_set_text_format_json_object},
    {"set_text_format_json_schema", cai_lua_params_set_text_format_json_schema},
    {"add_text", cai_lua_params_add_text},
    {"add_text_spooled", cai_lua_params_add_text_spooled},
    {"add_image_url", cai_lua_params_add_image_url},
    {"add_image_file_id", cai_lua_params_add_image_file_id},
    {"add_file_id", cai_lua_params_add_file_id},
    {"add_file_data_spooled", cai_lua_params_add_file_data_spooled},
    {"add_file_url", cai_lua_params_add_file_url},
    {"add_function_tool", cai_lua_params_add_function_tool},
    {"add_function_call_output", cai_lua_params_add_function_call_output},
    {"add_function_call_output_text",
     cai_lua_params_add_function_call_output_text},
    {"add_function_call_output_image_url",
     cai_lua_params_add_function_call_output_image_url},
    {"add_function_call_output_file_id",
     cai_lua_params_add_function_call_output_file_id},
    {"add_function_call_output_file_data_spooled",
     cai_lua_params_add_function_call_output_file_data_spooled},
    {"close", cai_lua_params_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_conversation_methods[] = {
    {"id", cai_lua_conversation_id},
    {"object", cai_lua_conversation_object},
    {"close", cai_lua_conversation_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_input_items_methods[] = {
    {"summary", cai_lua_input_items_summary},
    {"raw_json", cai_lua_input_items_raw_json},
    {"close", cai_lua_input_items_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_conversation_item_methods[] = {
    {"summary", cai_lua_conversation_item_summary},
    {"close", cai_lua_conversation_item_close},
    {NULL, NULL}};

static const luaL_Reg cai_lua_conversation_params_methods[] = {
    {"add_text", cai_lua_conversation_params_add_text},
    {"add_text_spooled", cai_lua_conversation_params_add_text_spooled},
    {"add_text_source", cai_lua_conversation_params_add_text_source},
    {"add_image_url", cai_lua_conversation_params_add_image_url},
    {"add_image_file_id", cai_lua_conversation_params_add_image_file_id},
    {"add_file_id", cai_lua_conversation_params_add_file_id},
    {"add_file_data_spooled",
     cai_lua_conversation_params_add_file_data_spooled},
    {"add_file_url", cai_lua_conversation_params_add_file_url},
    {"close", cai_lua_conversation_params_close},
    {NULL, NULL}};

static void cai_lua_metatable(lua_State *L, const char *name,
                              const luaL_Reg *methods, lua_CFunction gc) {
  luaL_newmetatable(L, name);
  lua_pushcfunction(L, gc);
  lua_setfield(L, -2, "__gc");
  lua_newtable(L);
  luaL_setfuncs(L, methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

int luaopen_cai(lua_State *L) {
#define CAI_LUA_SET_STRING(name, value)                                        \
  do {                                                                         \
    lua_pushstring(L, (value));                                                \
    lua_setfield(L, -2, (name));                                               \
  } while (0)
#define CAI_LUA_SET_INTEGER(name, value)                                       \
  do {                                                                         \
    lua_pushinteger(L, (lua_Integer)(value));                                  \
    lua_setfield(L, -2, (name));                                               \
  } while (0)
  cai_lua_metatable(L, CAI_LUA_CLIENT, cai_lua_client_methods,
                    cai_lua_client_gc);
  cai_lua_metatable(L, CAI_LUA_AGENT, cai_lua_agent_methods, cai_lua_agent_gc);
  cai_lua_metatable(L, CAI_LUA_SESSION, cai_lua_session_methods,
                    cai_lua_session_gc);
  cai_lua_metatable(L, CAI_LUA_RESPONSE, cai_lua_response_methods,
                    cai_lua_response_gc);
  cai_lua_metatable(L, CAI_LUA_OUTPUT, cai_lua_output_methods,
                    cai_lua_output_gc);
  cai_lua_metatable(L, CAI_LUA_REGISTRY, cai_lua_registry_methods,
                    cai_lua_registry_gc);
  cai_lua_metatable(L, CAI_LUA_MCP, cai_lua_mcp_methods, cai_lua_mcp_gc);
  cai_lua_metatable(L, CAI_LUA_SCHEMA, cai_lua_schema_methods,
                    cai_lua_schema_gc);
  cai_lua_metatable(L, CAI_LUA_PARAMS, cai_lua_params_methods,
                    cai_lua_params_gc);
  cai_lua_metatable(L, CAI_LUA_CONVERSATION, cai_lua_conversation_methods,
                    cai_lua_conversation_gc);
  cai_lua_metatable(L, CAI_LUA_INPUT_ITEMS, cai_lua_input_items_methods,
                    cai_lua_input_items_gc);
  cai_lua_metatable(L, CAI_LUA_CONVERSATION_ITEM,
                    cai_lua_conversation_item_methods,
                    cai_lua_conversation_item_gc);
  cai_lua_metatable(L, CAI_LUA_CONVERSATION_PARAMS,
                    cai_lua_conversation_params_methods,
                    cai_lua_conversation_params_gc);
  cai_lua_metatable(L, CAI_LUA_SPOOL_READER, cai_lua_spool_reader_methods,
                    cai_lua_spool_reader_gc);
  lua_newtable(L);
  lua_pushcfunction(L, cai_lua_open);
  lua_setfield(L, -2, "open");
  lua_pushcfunction(L, cai_lua_registry_new);
  lua_setfield(L, -2, "tool_registry");
  lua_pushcfunction(L, cai_lua_mcp_new);
  lua_setfield(L, -2, "mcp_handler");
  lua_pushcfunction(L, cai_lua_schema_new);
  lua_setfield(L, -2, "tool_schema");
  lua_pushcfunction(L, cai_lua_params_new);
  lua_setfield(L, -2, "response_params");
  lua_pushcfunction(L, cai_lua_conversation_params_new);
  lua_setfield(L, -2, "conversation_items_params");
  lua_pushcfunction(L, cai_lua_conversation_from_id);
  lua_setfield(L, -2, "conversation_from_id");
  lua_pushcfunction(L, cai_lua_load_dotenv_api_key);
  lua_setfield(L, -2, "load_dotenv_api_key");
  lua_pushcfunction(L, cai_lua_model_info);
  lua_setfield(L, -2, "model_info");
  lua_pushstring(L, CAI_SESSION_CONTINUITY_SERVER == 0 ? "server" : "server");
  lua_setfield(L, -2, "CONTINUITY_SERVER_NAME");
  CAI_LUA_SET_INTEGER("CONTINUITY_SERVER", CAI_SESSION_CONTINUITY_SERVER);
  CAI_LUA_SET_INTEGER("CONTINUITY_CLIENT_HISTORY",
                      CAI_SESSION_CONTINUITY_CLIENT_HISTORY);
  CAI_LUA_SET_INTEGER("CONTINUITY_AUTO", CAI_SESSION_CONTINUITY_AUTO);
  CAI_LUA_SET_STRING("DEFAULT_DOTENV_PATH", CAI_DEFAULT_DOTENV_PATH);
  CAI_LUA_SET_STRING("OPENAI_API_KEY_ENV", CAI_OPENAI_API_KEY_ENV);
  CAI_LUA_SET_STRING("OPENROUTER_API_KEY_ENV", CAI_OPENROUTER_API_KEY_ENV);
  CAI_LUA_SET_STRING("TOOL_CHOICE_AUTO", CAI_TOOL_CHOICE_AUTO);
  CAI_LUA_SET_STRING("TOOL_CHOICE_NONE", CAI_TOOL_CHOICE_NONE);
  CAI_LUA_SET_STRING("TOOL_CHOICE_REQUIRED", CAI_TOOL_CHOICE_REQUIRED);
  CAI_LUA_SET_STRING("REASONING_EFFORT_NONE", CAI_REASONING_EFFORT_NONE);
  CAI_LUA_SET_STRING("REASONING_EFFORT_MINIMAL", CAI_REASONING_EFFORT_MINIMAL);
  CAI_LUA_SET_STRING("REASONING_EFFORT_LOW", CAI_REASONING_EFFORT_LOW);
  CAI_LUA_SET_STRING("REASONING_EFFORT_MEDIUM", CAI_REASONING_EFFORT_MEDIUM);
  CAI_LUA_SET_STRING("REASONING_EFFORT_HIGH", CAI_REASONING_EFFORT_HIGH);
  CAI_LUA_SET_STRING("REASONING_EFFORT_XHIGH", CAI_REASONING_EFFORT_XHIGH);
  CAI_LUA_SET_STRING("REASONING_SUMMARY_AUTO", CAI_REASONING_SUMMARY_AUTO);
  CAI_LUA_SET_STRING("REASONING_SUMMARY_CONCISE",
                     CAI_REASONING_SUMMARY_CONCISE);
  CAI_LUA_SET_STRING("REASONING_SUMMARY_DETAILED",
                     CAI_REASONING_SUMMARY_DETAILED);
  CAI_LUA_SET_INTEGER("MODEL_CAP_RESPONSES", CAI_MODEL_CAP_RESPONSES);
  CAI_LUA_SET_INTEGER("MODEL_CAP_REALTIME", CAI_MODEL_CAP_REALTIME);
  CAI_LUA_SET_INTEGER("MODEL_CAP_STREAMING", CAI_MODEL_CAP_STREAMING);
  CAI_LUA_SET_INTEGER("MODEL_CAP_FUNCTION_CALLING",
                      CAI_MODEL_CAP_FUNCTION_CALLING);
  CAI_LUA_SET_INTEGER("MODEL_CAP_STRUCTURED_OUTPUTS",
                      CAI_MODEL_CAP_STRUCTURED_OUTPUTS);
  CAI_LUA_SET_INTEGER("MODEL_CAP_IMAGE_INPUT", CAI_MODEL_CAP_IMAGE_INPUT);
  CAI_LUA_SET_INTEGER("MODEL_CAP_AUDIO_INPUT", CAI_MODEL_CAP_AUDIO_INPUT);
  CAI_LUA_SET_INTEGER("MODEL_CAP_AUDIO_OUTPUT", CAI_MODEL_CAP_AUDIO_OUTPUT);
  CAI_LUA_SET_INTEGER("MODEL_META_VERIFIED", CAI_MODEL_META_VERIFIED);
  CAI_LUA_SET_INTEGER("MODEL_META_INCOMPLETE", CAI_MODEL_META_INCOMPLETE);
  CAI_LUA_SET_INTEGER("MODEL_META_INFERRED", CAI_MODEL_META_INFERRED);
  CAI_LUA_SET_INTEGER("MODEL_META_DEPRECATED", CAI_MODEL_META_DEPRECATED);
  CAI_LUA_SET_INTEGER("MODEL_META_PROVIDER_OPENROUTER",
                      CAI_MODEL_META_PROVIDER_OPENROUTER);
  CAI_LUA_SET_STRING("MODEL_DEFAULT_RESPONSES", CAI_MODEL_DEFAULT_RESPONSES);
  CAI_LUA_SET_STRING("MODEL_GPT_5_5", CAI_MODEL_GPT_5_5);
  CAI_LUA_SET_STRING("MODEL_GPT_5_5_2026_04_23", CAI_MODEL_GPT_5_5_2026_04_23);
  CAI_LUA_SET_STRING("MODEL_GPT_5_5_PRO", CAI_MODEL_GPT_5_5_PRO);
  CAI_LUA_SET_STRING("MODEL_GPT_5_5_PRO_2026_04_23",
                     CAI_MODEL_GPT_5_5_PRO_2026_04_23);
  CAI_LUA_SET_STRING("MODEL_GPT_5_4_NANO", CAI_MODEL_GPT_5_4_NANO);
  CAI_LUA_SET_STRING("MODEL_GPT_5_4_MINI", CAI_MODEL_GPT_5_4_MINI);
  CAI_LUA_SET_STRING("MODEL_GPT_5_4", CAI_MODEL_GPT_5_4);
  CAI_LUA_SET_STRING("MODEL_GPT_5_4_MINI_2026_03_17",
                     CAI_MODEL_GPT_5_4_MINI_2026_03_17);
  CAI_LUA_SET_STRING("MODEL_GPT_5_4_NANO_2026_03_17",
                     CAI_MODEL_GPT_5_4_NANO_2026_03_17);
  CAI_LUA_SET_STRING("MODEL_GPT_5_3_CHAT_LATEST",
                     CAI_MODEL_GPT_5_3_CHAT_LATEST);
  CAI_LUA_SET_STRING("MODEL_GPT_5_2", CAI_MODEL_GPT_5_2);
  CAI_LUA_SET_STRING("MODEL_GPT_5_2_2025_12_11", CAI_MODEL_GPT_5_2_2025_12_11);
  CAI_LUA_SET_STRING("MODEL_GPT_5_2_CHAT_LATEST",
                     CAI_MODEL_GPT_5_2_CHAT_LATEST);
  CAI_LUA_SET_STRING("MODEL_GPT_5_2_PRO", CAI_MODEL_GPT_5_2_PRO);
  CAI_LUA_SET_STRING("MODEL_GPT_5_2_PRO_2025_12_11",
                     CAI_MODEL_GPT_5_2_PRO_2025_12_11);
  CAI_LUA_SET_STRING("MODEL_GPT_5_1", CAI_MODEL_GPT_5_1);
  CAI_LUA_SET_STRING("MODEL_GPT_5_1_2025_11_13", CAI_MODEL_GPT_5_1_2025_11_13);
  CAI_LUA_SET_STRING("MODEL_GPT_5_1_CODEX", CAI_MODEL_GPT_5_1_CODEX);
  CAI_LUA_SET_STRING("MODEL_GPT_5_1_CODEX_MAX", CAI_MODEL_GPT_5_1_CODEX_MAX);
  CAI_LUA_SET_STRING("MODEL_GPT_5_1_MINI", CAI_MODEL_GPT_5_1_MINI);
  CAI_LUA_SET_STRING("MODEL_GPT_5_1_CHAT_LATEST",
                     CAI_MODEL_GPT_5_1_CHAT_LATEST);
  CAI_LUA_SET_STRING("MODEL_GPT_5", CAI_MODEL_GPT_5);
  CAI_LUA_SET_STRING("MODEL_GPT_5_MINI", CAI_MODEL_GPT_5_MINI);
  CAI_LUA_SET_STRING("MODEL_GPT_5_NANO", CAI_MODEL_GPT_5_NANO);
  CAI_LUA_SET_STRING("MODEL_GPT_5_2025_08_07", CAI_MODEL_GPT_5_2025_08_07);
  CAI_LUA_SET_STRING("MODEL_GPT_5_MINI_2025_08_07",
                     CAI_MODEL_GPT_5_MINI_2025_08_07);
  CAI_LUA_SET_STRING("MODEL_GPT_5_NANO_2025_08_07",
                     CAI_MODEL_GPT_5_NANO_2025_08_07);
  CAI_LUA_SET_STRING("MODEL_GPT_5_CHAT_LATEST", CAI_MODEL_GPT_5_CHAT_LATEST);
  CAI_LUA_SET_STRING("MODEL_GPT_5_CODEX", CAI_MODEL_GPT_5_CODEX);
  CAI_LUA_SET_STRING("MODEL_GPT_5_PRO", CAI_MODEL_GPT_5_PRO);
  CAI_LUA_SET_STRING("MODEL_GPT_5_PRO_2025_10_06",
                     CAI_MODEL_GPT_5_PRO_2025_10_06);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1", CAI_MODEL_GPT_4_1);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1_MINI", CAI_MODEL_GPT_4_1_MINI);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1_NANO", CAI_MODEL_GPT_4_1_NANO);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1_2025_04_14", CAI_MODEL_GPT_4_1_2025_04_14);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1_MINI_2025_04_14",
                     CAI_MODEL_GPT_4_1_MINI_2025_04_14);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1_NANO_2025_04_14",
                     CAI_MODEL_GPT_4_1_NANO_2025_04_14);
  CAI_LUA_SET_STRING("MODEL_O4_MINI", CAI_MODEL_O4_MINI);
  CAI_LUA_SET_STRING("MODEL_O4_MINI_2025_04_16", CAI_MODEL_O4_MINI_2025_04_16);
  CAI_LUA_SET_STRING("MODEL_O4_MINI_DEEP_RESEARCH",
                     CAI_MODEL_O4_MINI_DEEP_RESEARCH);
  CAI_LUA_SET_STRING("MODEL_O4_MINI_DEEP_RESEARCH_2025_06_26",
                     CAI_MODEL_O4_MINI_DEEP_RESEARCH_2025_06_26);
  CAI_LUA_SET_STRING("MODEL_O3", CAI_MODEL_O3);
  CAI_LUA_SET_STRING("MODEL_O3_2025_04_16", CAI_MODEL_O3_2025_04_16);
  CAI_LUA_SET_STRING("MODEL_O3_MINI", CAI_MODEL_O3_MINI);
  CAI_LUA_SET_STRING("MODEL_O3_MINI_2025_01_31", CAI_MODEL_O3_MINI_2025_01_31);
  CAI_LUA_SET_STRING("MODEL_O3_PRO", CAI_MODEL_O3_PRO);
  CAI_LUA_SET_STRING("MODEL_O3_PRO_2025_06_10", CAI_MODEL_O3_PRO_2025_06_10);
  CAI_LUA_SET_STRING("MODEL_O3_DEEP_RESEARCH", CAI_MODEL_O3_DEEP_RESEARCH);
  CAI_LUA_SET_STRING("MODEL_O3_DEEP_RESEARCH_2025_06_26",
                     CAI_MODEL_O3_DEEP_RESEARCH_2025_06_26);
  CAI_LUA_SET_STRING("MODEL_O1", CAI_MODEL_O1);
  CAI_LUA_SET_STRING("MODEL_O1_2024_12_17", CAI_MODEL_O1_2024_12_17);
  CAI_LUA_SET_STRING("MODEL_O1_PRO", CAI_MODEL_O1_PRO);
  CAI_LUA_SET_STRING("MODEL_O1_PRO_2025_03_19", CAI_MODEL_O1_PRO_2025_03_19);
  CAI_LUA_SET_STRING("MODEL_O1_PREVIEW", CAI_MODEL_O1_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_O1_PREVIEW_2024_09_12",
                     CAI_MODEL_O1_PREVIEW_2024_09_12);
  CAI_LUA_SET_STRING("MODEL_O1_MINI", CAI_MODEL_O1_MINI);
  CAI_LUA_SET_STRING("MODEL_O1_MINI_2024_09_12", CAI_MODEL_O1_MINI_2024_09_12);
  CAI_LUA_SET_STRING("MODEL_GPT_4O", CAI_MODEL_GPT_4O);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_2024_11_20", CAI_MODEL_GPT_4O_2024_11_20);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_2024_08_06", CAI_MODEL_GPT_4O_2024_08_06);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_2024_05_13", CAI_MODEL_GPT_4O_2024_05_13);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_AUDIO_PREVIEW",
                     CAI_MODEL_GPT_4O_AUDIO_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_AUDIO_PREVIEW_2024_10_01",
                     CAI_MODEL_GPT_4O_AUDIO_PREVIEW_2024_10_01);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_AUDIO_PREVIEW_2024_12_17",
                     CAI_MODEL_GPT_4O_AUDIO_PREVIEW_2024_12_17);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_AUDIO_PREVIEW_2025_06_03",
                     CAI_MODEL_GPT_4O_AUDIO_PREVIEW_2025_06_03);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_MINI_AUDIO_PREVIEW",
                     CAI_MODEL_GPT_4O_MINI_AUDIO_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_MINI_AUDIO_PREVIEW_2024_12_17",
                     CAI_MODEL_GPT_4O_MINI_AUDIO_PREVIEW_2024_12_17);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_SEARCH_PREVIEW",
                     CAI_MODEL_GPT_4O_SEARCH_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_MINI_SEARCH_PREVIEW",
                     CAI_MODEL_GPT_4O_MINI_SEARCH_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_SEARCH_PREVIEW_2025_03_11",
                     CAI_MODEL_GPT_4O_SEARCH_PREVIEW_2025_03_11);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_MINI_SEARCH_PREVIEW_2025_03_11",
                     CAI_MODEL_GPT_4O_MINI_SEARCH_PREVIEW_2025_03_11);
  CAI_LUA_SET_STRING("MODEL_CHATGPT_4O_LATEST", CAI_MODEL_CHATGPT_4O_LATEST);
  CAI_LUA_SET_STRING("MODEL_CODEX_MINI_LATEST", CAI_MODEL_CODEX_MINI_LATEST);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_MINI", CAI_MODEL_GPT_4O_MINI);
  CAI_LUA_SET_STRING("MODEL_GPT_4O_MINI_2024_07_18",
                     CAI_MODEL_GPT_4O_MINI_2024_07_18);
  CAI_LUA_SET_STRING("MODEL_GPT_4_TURBO", CAI_MODEL_GPT_4_TURBO);
  CAI_LUA_SET_STRING("MODEL_GPT_4_TURBO_2024_04_09",
                     CAI_MODEL_GPT_4_TURBO_2024_04_09);
  CAI_LUA_SET_STRING("MODEL_GPT_4_0125_PREVIEW", CAI_MODEL_GPT_4_0125_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4_TURBO_PREVIEW",
                     CAI_MODEL_GPT_4_TURBO_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4_1106_PREVIEW", CAI_MODEL_GPT_4_1106_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4_VISION_PREVIEW",
                     CAI_MODEL_GPT_4_VISION_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_GPT_4", CAI_MODEL_GPT_4);
  CAI_LUA_SET_STRING("MODEL_GPT_4_0314", CAI_MODEL_GPT_4_0314);
  CAI_LUA_SET_STRING("MODEL_GPT_4_0613", CAI_MODEL_GPT_4_0613);
  CAI_LUA_SET_STRING("MODEL_GPT_4_32K", CAI_MODEL_GPT_4_32K);
  CAI_LUA_SET_STRING("MODEL_GPT_4_32K_0314", CAI_MODEL_GPT_4_32K_0314);
  CAI_LUA_SET_STRING("MODEL_GPT_4_32K_0613", CAI_MODEL_GPT_4_32K_0613);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO", CAI_MODEL_GPT_3_5_TURBO);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO_16K", CAI_MODEL_GPT_3_5_TURBO_16K);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO_0301", CAI_MODEL_GPT_3_5_TURBO_0301);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO_0613", CAI_MODEL_GPT_3_5_TURBO_0613);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO_1106", CAI_MODEL_GPT_3_5_TURBO_1106);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO_0125", CAI_MODEL_GPT_3_5_TURBO_0125);
  CAI_LUA_SET_STRING("MODEL_GPT_3_5_TURBO_16K_0613",
                     CAI_MODEL_GPT_3_5_TURBO_16K_0613);
  CAI_LUA_SET_STRING("MODEL_COMPUTER_USE_PREVIEW",
                     CAI_MODEL_COMPUTER_USE_PREVIEW);
  CAI_LUA_SET_STRING("MODEL_COMPUTER_USE_PREVIEW_2025_03_11",
                     CAI_MODEL_COMPUTER_USE_PREVIEW_2025_03_11);
  CAI_LUA_SET_STRING(
      "OPENROUTER_MODEL_NVIDIA_NEMOTRON_3_NANO_OMNI_30B_A3B_REASONING_FREE",
      CAI_OPENROUTER_MODEL_NVIDIA_NEMOTRON_3_NANO_OMNI_30B_A3B_REASONING_FREE);
  CAI_LUA_SET_STRING("OPENROUTER_MODEL_FREE_ROUTER",
                     CAI_OPENROUTER_MODEL_FREE_ROUTER);
  CAI_LUA_SET_STRING("OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE",
                     CAI_OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE);
  CAI_LUA_SET_STRING("OPENROUTER_MODEL_DEFAULT_RESPONSES",
                     CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES);
  CAI_LUA_SET_STRING("MCP_PROTOCOL_VERSION", CAI_MCP_PROTOCOL_VERSION);
#undef CAI_LUA_SET_INTEGER
#undef CAI_LUA_SET_STRING
  return 1;
}
