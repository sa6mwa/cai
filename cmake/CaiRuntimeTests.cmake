# Inspect the current CMake graph, never stale outputs from disabled products.
if(NOT CAI_BUILD_TESTS OR NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CAI_TARGET_ID STREQUAL "x86_64-linux-gnu")
  return()
endif()

get_property(_cai_build_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_cai_runtime_targets "")
foreach(_target IN LISTS _cai_build_targets)
  get_target_property(_type "${_target}" TYPE)
  if(_type STREQUAL "EXECUTABLE")
    list(APPEND _cai_runtime_targets "$<TARGET_FILE:${_target}>")
  endif()
endforeach()
if(NOT _cai_runtime_targets)
  message(FATAL_ERROR "no configured executables for Bootlin runtime verification")
endif()
cai_local_dependency_runtime_dirs(_cai_test_runtime_dirs)
add_test(NAME cai_local_bootlin_runtime_test
  COMMAND "${CMAKE_COMMAND}"
    "-DCAI_RUNTIME_TARGETS=${_cai_runtime_targets}"
    "-DCAI_READELF=${CMAKE_READELF}"
    "-DCAI_SYSROOT=${CMAKE_SYSROOT}"
    "-DCAI_DEPENDENCY_RUNTIME_DIRS=${_cai_test_runtime_dirs}"
    -P "${CMAKE_CURRENT_LIST_DIR}/../tests/local_bootlin_runtime_test.cmake")
set_tests_properties(cai_local_bootlin_runtime_test PROPERTIES
  LABELS "packaging;local" TIMEOUT 3)
