#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

#if LUA_VERSION_RELEASE_NUM != 50501
#error "CAI Lua verification requires Lua 5.5.1"
#endif

static int traceback(lua_State *state) {
  const char *message = lua_tostring(state, 1);
  luaL_traceback(state, state, message != NULL ? message : "Lua error", 1);
  return 1;
}

int main(int argc, char **argv) {
  lua_State *state;
  int status;
  int index;
  int script;
  int expression;
  if (argc == 2 &&
      (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
    puts(LUA_RELEASE);
    return 0;
  }
  expression = argc > 1 && strcmp(argv[1], "-e") == 0;
  script = expression ? 2 : 1;
  if (argc <= script) {
    fprintf(stderr, "usage: %s SCRIPT [ARG ...] | -e CODE [ARG ...] | -v\n",
            argv[0]);
    return 2;
  }
  state = luaL_newstate();
  if (state == NULL) {
    fputs("could not create Lua state\n", stderr);
    return 1;
  }
  luaL_openlibs(state);
  lua_createtable(state, argc - script - 1, script + 1);
  for (index = 0; index < argc; ++index) {
    lua_pushstring(state, argv[index]);
    lua_rawseti(state, -2, index - script);
  }
  lua_setglobal(state, "arg");
  lua_pushcfunction(state, traceback);
  status = expression ? luaL_loadstring(state, argv[script])
                      : luaL_loadfile(state, argv[script]);
  if (status == LUA_OK) {
    for (index = script + 1; index < argc; ++index)
      lua_pushstring(state, argv[index]);
    status = lua_pcall(state, argc - script - 1, LUA_MULTRET, 1);
  }
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(state, -1));
  lua_close(state);
  return status == LUA_OK ? 0 : 1;
}
