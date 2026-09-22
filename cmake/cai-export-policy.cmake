# Enforce the source-controlled public dynamic-symbol boundary for shipped
# shared libraries and modules.  Keep the platform-specific linker artifacts
# in the build directory; the allowlists are the reviewed source of truth.
function(cai_apply_export_policy target allowlist)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "cai_apply_export_policy target does not exist: ${target}")
  endif()
  if(NOT EXISTS "${allowlist}")
    message(FATAL_ERROR "cai export allowlist does not exist: ${allowlist}")
  endif()

  file(STRINGS "${allowlist}" _cai_export_symbols)
  if(NOT _cai_export_symbols)
    message(FATAL_ERROR "cai export allowlist is empty: ${allowlist}")
  endif()
  set(_cai_seen_symbols "")
  foreach(_cai_export_symbol IN LISTS _cai_export_symbols)
    if(NOT _cai_export_symbol MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
      message(FATAL_ERROR
        "cai export allowlist has an invalid symbol '${_cai_export_symbol}': "
        "${allowlist}")
    endif()
    list(FIND _cai_seen_symbols "${_cai_export_symbol}" _cai_symbol_index)
    if(NOT _cai_symbol_index EQUAL -1)
      message(FATAL_ERROR
        "cai export allowlist has a duplicate symbol '${_cai_export_symbol}': "
        "${allowlist}")
    endif()
    list(APPEND _cai_seen_symbols "${_cai_export_symbol}")
  endforeach()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_cai_export_linker_file
      "${CMAKE_CURRENT_BINARY_DIR}/${target}.export-map")
    file(WRITE "${_cai_export_linker_file}" "{\n  global:\n")
    foreach(_cai_export_symbol IN LISTS _cai_export_symbols)
      file(APPEND "${_cai_export_linker_file}"
        "    ${_cai_export_symbol};\n")
    endforeach()
    file(APPEND "${_cai_export_linker_file}" "  local:\n    *;\n};\n")
    target_link_options(${target} PRIVATE
      "-Wl,--version-script=${_cai_export_linker_file}")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_cai_export_linker_file
      "${CMAKE_CURRENT_BINARY_DIR}/${target}.exported-symbols")
    file(WRITE "${_cai_export_linker_file}" "")
    foreach(_cai_export_symbol IN LISTS _cai_export_symbols)
      file(APPEND "${_cai_export_linker_file}" "_${_cai_export_symbol}\n")
    endforeach()
    target_link_options(${target} PRIVATE
      "-Wl,-exported_symbols_list,${_cai_export_linker_file}")
  else()
    message(FATAL_ERROR
      "cai export policy supports Linux and Darwin; got ${CMAKE_SYSTEM_NAME}")
  endif()
endfunction()
