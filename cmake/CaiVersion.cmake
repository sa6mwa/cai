function(cai_detect_version out_var)
  set(_cai_version "0.0.0")

  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "v[0-9]*"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      RESULT_VARIABLE _cai_git_result
      OUTPUT_VARIABLE _cai_git_tag
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_cai_git_result EQUAL 0 AND _cai_git_tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+.*)$")
      string(REGEX REPLACE "^v" "" _cai_version "${_cai_git_tag}")
    endif()
  endif()

  if(DEFINED CAI_VERSION_OVERRIDE AND NOT CAI_VERSION_OVERRIDE STREQUAL "")
    set(_cai_version "${CAI_VERSION_OVERRIDE}")
  endif()

  set(${out_var} "${_cai_version}" PARENT_SCOPE)
endfunction()
