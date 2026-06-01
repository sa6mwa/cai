function(cai_get_imported_target_location target out_var)
  set(_cai_location "")
  foreach(_cai_prop IN ITEMS
      IMPORTED_LOCATION
      IMPORTED_LOCATION_RELEASE
      IMPORTED_LOCATION_RELWITHDEBINFO
      IMPORTED_LOCATION_MINSIZEREL
      IMPORTED_LOCATION_DEBUG
      IMPORTED_LOCATION_NOCONFIG)
    get_target_property(_cai_candidate "${target}" "${_cai_prop}")
    if(_cai_candidate AND NOT _cai_candidate STREQUAL "_cai_candidate-NOTFOUND")
      set(_cai_location "${_cai_candidate}")
      break()
    endif()
  endforeach()
  set(${out_var} "${_cai_location}" PARENT_SCOPE)
endfunction()

function(cai_lonejson_expected_soname abi_version out_var)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_cai_expected "liblonejson.${abi_version}.dylib")
  else()
    set(_cai_expected "liblonejson.so.${abi_version}")
  endif()
  set(${out_var} "${_cai_expected}" PARENT_SCOPE)
endfunction()

function(cai_probe_lonejson_library_abi library_path abi_version out_var reason_var)
  set(_cai_match FALSE)
  set(_cai_reason "")

  if(NOT library_path OR library_path STREQUAL "" OR NOT EXISTS "${library_path}")
    set(_cai_reason
        "lonejson library path is missing or does not exist: ${library_path}")
    set(${out_var} FALSE PARENT_SCOPE)
    set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
    return()
  endif()

  get_filename_component(_cai_library_ext "${library_path}" EXT)
  if(_cai_library_ext STREQUAL ".a")
    set(${out_var} TRUE PARENT_SCOPE)
    set(${reason_var} "" PARENT_SCOPE)
    return()
  endif()

  cai_lonejson_expected_soname("${abi_version}" _cai_expected_soname)

  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_program(_cai_otool NAMES otool llvm-otool)
    if(NOT _cai_otool)
      set(_cai_reason
          "unable to validate lonejson ABI because neither otool nor "
          "llvm-otool is available")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
    execute_process(
      COMMAND "${_cai_otool}" -D "${library_path}"
      RESULT_VARIABLE _cai_otool_result
      OUTPUT_VARIABLE _cai_otool_output
      ERROR_VARIABLE _cai_otool_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _cai_otool_result EQUAL 0)
      set(_cai_reason
          "failed to inspect lonejson install name for ${library_path}:\n"
          "${_cai_otool_error}\n${_cai_otool_output}")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
    string(REGEX MATCH "[^\n]+${_cai_expected_soname}"
      _cai_install_name_match "${_cai_otool_output}")
    if(_cai_install_name_match STREQUAL "")
      set(_cai_reason
          "discovered lonejson library ${library_path} does not advertise "
          "ABI ${abi_version} (${_cai_expected_soname})")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
  else()
    find_program(_cai_readelf NAMES readelf llvm-readelf eu-readelf)
    find_program(_cai_objdump NAMES objdump llvm-objdump)
    if(_cai_readelf)
      execute_process(
        COMMAND "${_cai_readelf}" -d "${library_path}"
        RESULT_VARIABLE _cai_inspect_result
        OUTPUT_VARIABLE _cai_inspect_output
        ERROR_VARIABLE _cai_inspect_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      string(REGEX MATCH "SONAME[^[]*\\[([^]]+)\\]"
        _cai_soname_match "${_cai_inspect_output}")
      set(_cai_actual_soname "${CMAKE_MATCH_1}")
    elseif(_cai_objdump)
      execute_process(
        COMMAND "${_cai_objdump}" -p "${library_path}"
        RESULT_VARIABLE _cai_inspect_result
        OUTPUT_VARIABLE _cai_inspect_output
        ERROR_VARIABLE _cai_inspect_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      string(REGEX MATCH "SONAME[^\n]* ([^ \n]+)"
        _cai_soname_match "${_cai_inspect_output}")
      set(_cai_actual_soname "${CMAKE_MATCH_1}")
    else()
      set(_cai_reason
          "unable to validate lonejson ABI because neither readelf nor "
          "objdump is available")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
    if(NOT _cai_inspect_result EQUAL 0)
      set(_cai_reason
          "failed to inspect lonejson SONAME for ${library_path}:\n"
          "${_cai_inspect_error}\n${_cai_inspect_output}")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
    if(_cai_actual_soname STREQUAL "")
      set(_cai_reason
          "could not determine lonejson SONAME for ${library_path}")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
    if(NOT _cai_actual_soname STREQUAL "${_cai_expected_soname}")
      set(_cai_reason
          "discovered lonejson SONAME ${_cai_actual_soname} does not match "
          "required ABI ${abi_version} (${_cai_expected_soname})")
      set(${out_var} FALSE PARENT_SCOPE)
      set(${reason_var} "${_cai_reason}" PARENT_SCOPE)
      return()
    endif()
  endif()

  set(${out_var} TRUE PARENT_SCOPE)
  set(${reason_var} "" PARENT_SCOPE)
endfunction()

function(cai_validate_lonejson_library_abi library_path abi_version context)
  cai_probe_lonejson_library_abi(
    "${library_path}" "${abi_version}" _cai_match _cai_reason)
  if(NOT _cai_match)
    message(FATAL_ERROR "${context}: ${_cai_reason}")
  endif()
endfunction()
