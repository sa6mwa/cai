function(cai_detect_version out_var source_dir)
  set(_cai_version "0.0.0")
  set(_cai_semver_regex
      "^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)([.](0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*))?(\\+([0-9A-Za-z-]+([.][0-9A-Za-z-]+)*))?$")
  set(_cai_version_override "")
  if(DEFINED CAI_VERSION_OVERRIDE AND NOT CAI_VERSION_OVERRIDE STREQUAL "")
    set(_cai_version_override "${CAI_VERSION_OVERRIDE}")
  elseif(DEFINED ENV{CAI_VERSION_OVERRIDE} AND NOT "$ENV{CAI_VERSION_OVERRIDE}" STREQUAL "")
    set(_cai_version_override "$ENV{CAI_VERSION_OVERRIDE}")
  endif()

  if(NOT _cai_version_override STREQUAL "")
    set(_cai_version "${_cai_version_override}")
  else()
    set(_cai_version_script "${source_dir}/scripts/release_version.sh")
    if(NOT EXISTS "${_cai_version_script}")
      message(FATAL_ERROR
        "cai version resolver is missing: ${_cai_version_script}")
    endif()
    execute_process(
      COMMAND "${_cai_version_script}" "${source_dir}"
      RESULT_VARIABLE _cai_version_result
      OUTPUT_VARIABLE _cai_version_output
      ERROR_VARIABLE _cai_version_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _cai_version_result EQUAL 0)
      message(FATAL_ERROR
        "cai version resolver failed:\n${_cai_version_error}")
    endif()
    set(_cai_version "${_cai_version_output}")
  endif()

  if(NOT _cai_version MATCHES "${_cai_semver_regex}")
    message(FATAL_ERROR
      "invalid cai release version: ${_cai_version}")
  endif()

  set(${out_var} "${_cai_version}" PARENT_SCOPE)
endfunction()
