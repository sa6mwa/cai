if(NOT DEFINED CAI_SOURCE_DIR)
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()

set(_cai_presets "${CAI_SOURCE_DIR}/CMakePresets.json")
if(NOT EXISTS "${_cai_presets}")
  message(FATAL_ERROR "CMakePresets.json not found")
endif()

file(READ "${_cai_presets}" _cai_presets_text)
if(NOT _cai_presets_text MATCHES "\"name\"[ \t\r\n]*:[ \t\r\n]*\"base\"")
  message(FATAL_ERROR "base CMake preset is missing")
endif()
if(NOT _cai_presets_text MATCHES
   "\"CAI_DEPENDENCY_MODE\"[ \t\r\n]*:[ \t\r\n]*\"cpkt\"")
  message(FATAL_ERROR
    "base CMake preset must pin CAI_DEPENDENCY_MODE=cpkt so stale build "
    "cache state cannot change sanitizer or fuzz dependency resolution")
endif()

set(_cai_x86_release_target_id "")
set(_cai_host_release_presets
    x86_64-linux-musl-host-release
    aarch64-linux-gnu-host-release
    aarch64-linux-musl-host-release
    armhf-linux-gnu-host-release
    armhf-linux-musl-host-release
    arm64-apple-darwin-host-release
    x86_64-apple-darwin-host-release)
string(JSON _cai_configure_preset_count LENGTH "${_cai_presets_text}" configurePresets)
math(EXPR _cai_configure_preset_last "${_cai_configure_preset_count} - 1")
foreach(_cai_preset_index RANGE 0 ${_cai_configure_preset_last})
  string(JSON _cai_preset_name GET "${_cai_presets_text}" configurePresets
         ${_cai_preset_index} name)
  if(_cai_preset_name STREQUAL "release")
    string(JSON _cai_release_target_id ERROR_VARIABLE _cai_release_target_id_error
           GET "${_cai_presets_text}" configurePresets ${_cai_preset_index}
           cacheVariables CAI_TARGET_ID)
    if(_cai_release_target_id_error OR
       NOT _cai_release_target_id STREQUAL "x86_64-linux-gnu")
      message(FATAL_ERROR
        "release preset must retain the baseline x86_64-linux-gnu release "
        "target id used by the release lifecycle")
    endif()
  elseif(_cai_preset_name STREQUAL "x86_64-linux-gnu-release")
    string(JSON _cai_x86_release_target_id ERROR_VARIABLE _cai_x86_target_id_error
           GET "${_cai_presets_text}" configurePresets ${_cai_preset_index}
           cacheVariables CAI_TARGET_ID)
    if(_cai_x86_target_id_error)
      set(_cai_x86_release_target_id "")
    endif()
  endif()
  list(FIND _cai_host_release_presets "${_cai_preset_name}"
       _cai_host_preset_index)
  if(NOT _cai_host_preset_index EQUAL -1)
    string(JSON _cai_host_compiler ERROR_VARIABLE _cai_host_compiler_error
           GET "${_cai_presets_text}" configurePresets ${_cai_preset_index}
           cacheVariables CMAKE_C_COMPILER)
    if(NOT _cai_host_compiler_error)
      message(FATAL_ERROR
        "${_cai_preset_name} must use the native host compiler and must not "
        "set CMAKE_C_COMPILER")
    endif()
    string(JSON _cai_host_target_id ERROR_VARIABLE _cai_host_target_id_error
           GET "${_cai_presets_text}" configurePresets ${_cai_preset_index}
           cacheVariables CAI_TARGET_ID)
    if(_cai_host_target_id_error OR _cai_host_target_id STREQUAL "")
      message(FATAL_ERROR "${_cai_preset_name} must set CAI_TARGET_ID")
    endif()
    list(REMOVE_ITEM _cai_host_release_presets "${_cai_preset_name}")
  endif()
endforeach()

if(NOT _cai_x86_release_target_id STREQUAL "")
  message(FATAL_ERROR
    "x86_64-linux-gnu-release preset should inherit the release preset target id")
endif()
if(_cai_host_release_presets)
  message(FATAL_ERROR
    "missing host-native release presets: ${_cai_host_release_presets}")
endif()
