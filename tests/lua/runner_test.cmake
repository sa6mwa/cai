execute_process(COMMAND "${CAI_LUA_RUNNER}" -v
  RESULT_VARIABLE result OUTPUT_VARIABLE output)
if(NOT result EQUAL 0 OR NOT output STREQUAL "Lua 5.5.1\n")
  message(FATAL_ERROR "Unexpected Lua runner version: ${output}")
endif()
execute_process(COMMAND "${CAI_LUA_RUNNER}" -e
  "assert(arg[1] == 'hello'); assert((...) == 'hello'); assert(os.execute('true')); print('ok')"
  hello RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output STREQUAL "ok\n")
  message(FATAL_ERROR "Lua arguments/subprocess failed: ${output}\n${error}")
endif()
set(many_arguments)
foreach(index RANGE 1 100)
  list(APPEND many_arguments "argument-${index}")
endforeach()
execute_process(COMMAND "${CAI_LUA_RUNNER}" -e
  "assert(select('#', ...) == 100); print('many arguments ok')"
  ${many_arguments}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output STREQUAL "many arguments ok\n")
  message(FATAL_ERROR "Lua runner must support many arguments: ${output}\n${error}")
endif()
foreach(code IN ITEMS "error('runner sentinel')" "local =")
  execute_process(COMMAND "${CAI_LUA_RUNNER}" -e "${code}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 1 OR error STREQUAL "")
    message(FATAL_ERROR "Lua errors must produce diagnostics and exit 1")
  endif()
endforeach()
execute_process(COMMAND "${CAI_LUA_RUNNER}"
  RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 2 OR NOT error MATCHES "usage:")
  message(FATAL_ERROR "Missing script must produce usage and exit 2")
endif()
