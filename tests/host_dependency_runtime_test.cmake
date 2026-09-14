function(run)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${CAI_SOURCE_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Host dependency runtime verification failed:\n${output}\n${error}")
  endif()
endfunction()

foreach(mode IN ITEMS host auto)
  set(build_dir "${CAI_BINARY_DIR}/dependency-runtime-${mode}")
  # A fresh configuration prevents cpkt-only variables masking regressions.
  file(REMOVE_RECURSE "${build_dir}")
  string(REPLACE ";" "\\;" prefixes "${CAI_PREFIXES}")
  run("${CMAKE_COMMAND}" --preset debug-host-deps
    -S "${CAI_SOURCE_DIR}" -B "${build_dir}"
    "-DCAI_DEPENDENCY_MODE=${mode}" "-DCMAKE_PREFIX_PATH=${prefixes}")
  file(READ "${build_dir}/cai.pc" metadata)
  if(NOT metadata MATCHES "dependency_mode=host")
    message(FATAL_ERROR "${mode} did not resolve to host dependencies")
  endif()
  run("${CMAKE_COMMAND}" --build "${build_dir}"
    --target cai_example_basic_response)
  run("${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}"
    -R "^cai_local_bootlin_runtime_test$" --output-on-failure)
  run("${build_dir}/cai_example_basic_response" --help)
endforeach()
