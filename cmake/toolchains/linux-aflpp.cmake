set(CPKT_TARGET_ID "x86_64-linux-gnu" CACHE STRING
    "pkt.systems lifecycle target id" FORCE)

include("${CMAKE_CURRENT_LIST_DIR}/linux-bootlin.cmake")

set(_cpkt_aflpp_script "${CMAKE_CURRENT_LIST_DIR}/../../scripts/cpkt-aflpp.sh")
if(NOT EXISTS "${_cpkt_aflpp_script}")
  message(FATAL_ERROR "missing lifecycle AFL++ resolver: ${_cpkt_aflpp_script}")
endif()

execute_process(
  COMMAND "${_cpkt_aflpp_script}" discover
  RESULT_VARIABLE _cpkt_aflpp_result
  OUTPUT_VARIABLE _cpkt_aflpp_description
  ERROR_VARIABLE _cpkt_aflpp_error)
if(NOT _cpkt_aflpp_result EQUAL 0)
  message(FATAL_ERROR
    "failed to provision pinned native AFL++ instrumentation:\n"
    "${_cpkt_aflpp_error}")
endif()

function(_cpkt_aflpp_value key out_var)
  string(REGEX MATCH "(^|\n)${key}=([^\n]*)" _match
         "${_cpkt_aflpp_description}")
  if(NOT _match)
    message(FATAL_ERROR "pinned AFL++ description is missing ${key}")
  endif()
  set(${out_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

_cpkt_aflpp_value(cc _cpkt_aflpp_cc)
_cpkt_aflpp_value(cxx _cpkt_aflpp_cxx)
_cpkt_aflpp_value(helper _cpkt_aflpp_helper)

set(ENV{AFL_PATH} "${_cpkt_aflpp_helper}")
set(CMAKE_C_COMPILER "${_cpkt_aflpp_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_cpkt_aflpp_cxx}" CACHE FILEPATH "" FORCE)
