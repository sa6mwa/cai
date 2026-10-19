if(NOT DEFINED CAI_SOURCE_DIR)
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR)
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()

set(_cai_presets "${CAI_SOURCE_DIR}/CMakePresets.json")
if(NOT EXISTS "${_cai_presets}")
  message(FATAL_ERROR "CMakePresets.json not found")
endif()

file(READ "${_cai_presets}" _cai_presets_text)
string(JSON _cai_preset_schema GET "${_cai_presets_text}" version)
if(_cai_preset_schema GREATER 3)
  message(FATAL_ERROR "presets must remain readable by the declared CMake 3.21 minimum")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --list-presets=all
  WORKING_DIRECTORY "${CAI_SOURCE_DIR}"
  RESULT_VARIABLE _cai_preset_parse_result
  OUTPUT_VARIABLE _cai_preset_parse_stdout
  ERROR_VARIABLE _cai_preset_parse_stderr)
if(NOT _cai_preset_parse_result EQUAL 0)
  message(FATAL_ERROR "CMake rejected preset fields for their schema: ${_cai_preset_parse_stderr}")
endif()
set(_cai_source_dir_literal "\${sourceDir}")
if(NOT _cai_presets_text MATCHES
   "\"CAI_DEPENDENCY_MODE\"[ \t\r\n]*:[ \t\r\n]*\"cpkt\"")
  message(FATAL_ERROR "base preset must pin CAI_DEPENDENCY_MODE=cpkt")
endif()
if(_cai_presets_text MATCHES
   "\"CMAKE_C_COMPILER\"|\"name\"[ \t\r\n]*:[ \t\r\n]*\"(tsan|msan)\"|clang")
  message(FATAL_ERROR
    "CMake presets must not select a Clang compiler or retain TSan/MSan paths")
endif()

foreach(_cai_deprecated_mode IN ITEMS lockdc pkt)
  set(_cai_mode_build_dir
      "${CAI_BINARY_DIR}/deprecated-dependency-mode-${_cai_deprecated_mode}")
  file(REMOVE_RECURSE "${_cai_mode_build_dir}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${CAI_SOURCE_DIR}"
            -B "${_cai_mode_build_dir}" -G Ninja
            -DCAI_TARGET_ID=x86_64-linux-gnu
            "-DCAI_DEPENDENCY_MODE=${_cai_deprecated_mode}"
    RESULT_VARIABLE _cai_mode_result
    OUTPUT_VARIABLE _cai_mode_stdout
    ERROR_VARIABLE _cai_mode_stderr)
  if(_cai_mode_result EQUAL 0 OR
     NOT _cai_mode_stderr MATCHES
     "CAI_DEPENDENCY_MODE must be one of: cpkt, host, auto\\.")
    message(FATAL_ERROR
      "CMake must reject deprecated dependency mode "
      "${_cai_deprecated_mode}. stdout:\n${_cai_mode_stdout}\n"
      "stderr:\n${_cai_mode_stderr}")
  endif()
endforeach()

function(_cai_find_configure_preset name out_index)
  string(JSON _count LENGTH "${_cai_presets_text}" configurePresets)
  math(EXPR _last "${_count} - 1")
  foreach(_index RANGE 0 ${_last})
    string(JSON _candidate GET "${_cai_presets_text}" configurePresets ${_index} name)
    if(_candidate STREQUAL "${name}")
      set(${out_index} ${_index} PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "required lifecycle configure preset is missing: ${name}")
endfunction()

function(_cai_require_cache_value preset_index preset_name key expected)
  string(JSON _value ERROR_VARIABLE _error GET "${_cai_presets_text}"
         configurePresets ${preset_index} cacheVariables ${key})
  if(_error OR NOT _value STREQUAL "${expected}")
    message(FATAL_ERROR
      "${preset_name} must set ${key}=${expected}; got ${_value}")
  endif()
endfunction()

function(_cai_reject_cache_key preset_index preset_name key)
  string(JSON _value ERROR_VARIABLE _error GET "${_cai_presets_text}"
         configurePresets ${preset_index} cacheVariables ${key})
  if(NOT _error)
    message(FATAL_ERROR
      "${preset_name} must not set ${key}; got ${_value}")
  endif()
endfunction()

function(_cai_require_inherits preset_index preset_name parent)
  string(JSON _inherits ERROR_VARIABLE _error GET "${_cai_presets_text}"
         configurePresets ${preset_index} inherits)
  if(_error OR NOT _inherits STREQUAL "${parent}")
    message(FATAL_ERROR "${preset_name} must inherit ${parent}")
  endif()
endfunction()

_cai_find_configure_preset(bootlin-native _cai_bootlin_native_index)
_cai_require_cache_value(${_cai_bootlin_native_index} bootlin-native
  CMAKE_TOOLCHAIN_FILE "${_cai_source_dir_literal}/cmake/toolchains/linux-bootlin.cmake")
_cai_require_cache_value(${_cai_bootlin_native_index} bootlin-native
  CPKT_TARGET_ID x86_64-linux-gnu)

_cai_find_configure_preset(native-lifecycle _cai_native_lifecycle_index)
_cai_require_inherits(${_cai_native_lifecycle_index} native-lifecycle base)
_cai_require_cache_value(${_cai_native_lifecycle_index} native-lifecycle
  CMAKE_TOOLCHAIN_FILE "${_cai_source_dir_literal}/cmake/toolchains/native-lifecycle.cmake")
_cai_reject_cache_key(${_cai_native_lifecycle_index} native-lifecycle
  CPKT_TARGET_ID)

_cai_find_configure_preset(release-base _cai_release_base_index)
_cai_require_inherits(${_cai_release_base_index} release-base base)
_cai_require_cache_value(${_cai_release_base_index} release-base
  CMAKE_BUILD_TYPE Release)
_cai_reject_cache_key(${_cai_release_base_index} release-base
  CPKT_TARGET_ID)
_cai_reject_cache_key(${_cai_release_base_index} release-base
  CMAKE_TOOLCHAIN_FILE)

_cai_find_configure_preset(debug _cai_debug_index)
_cai_require_inherits(${_cai_debug_index} debug native-lifecycle)

foreach(_cai_native_preset IN ITEMS release asan)
  _cai_find_configure_preset(${_cai_native_preset} _cai_native_index)
  _cai_require_inherits(${_cai_native_index} ${_cai_native_preset} bootlin-native)
endforeach()
foreach(_cai_debug_child IN ITEMS debug-lua coverage integration)
  _cai_find_configure_preset(${_cai_debug_child} _cai_debug_child_index)
  _cai_require_inherits(${_cai_debug_child_index} ${_cai_debug_child} debug)
endforeach()

_cai_find_configure_preset(valgrind _cai_valgrind_index)
_cai_require_inherits(${_cai_valgrind_index} valgrind debug)
string(JSON _cai_valgrind_build_dir GET "${_cai_presets_text}" configurePresets ${_cai_valgrind_index} binaryDir)
if(NOT _cai_valgrind_build_dir STREQUAL "${_cai_source_dir_literal}/build/debug")
  message(FATAL_ERROR "Valgrind must reuse the native Debug producer")
endif()
_cai_require_cache_value(${_cai_valgrind_index} valgrind
  CAI_ENABLE_VALGRIND_TESTS ON)
_cai_find_configure_preset(fuzz _cai_fuzz_index)
_cai_require_cache_value(${_cai_fuzz_index} fuzz
  CMAKE_TOOLCHAIN_FILE "${_cai_source_dir_literal}/cmake/toolchains/linux-aflpp.cmake")
_cai_require_cache_value(${_cai_fuzz_index} fuzz CPKT_TARGET_ID x86_64-linux-gnu)

foreach(_cai_linux_release_preset IN ITEMS
    x86_64-linux-gnu-release
    x86_64-linux-musl-release
    aarch64-linux-gnu-release
    aarch64-linux-musl-release
    armhf-linux-gnu-release
    armhf-linux-musl-release)
  _cai_find_configure_preset(${_cai_linux_release_preset} _cai_release_index)
  _cai_require_cache_value(${_cai_release_index} ${_cai_linux_release_preset}
    CMAKE_TOOLCHAIN_FILE "${_cai_source_dir_literal}/cmake/toolchains/linux-bootlin.cmake")
endforeach()
_cai_find_configure_preset(arm64-apple-darwin-release _cai_darwin_release_index)
_cai_require_inherits(${_cai_darwin_release_index}
  arm64-apple-darwin-release release-base)
_cai_require_cache_value(${_cai_darwin_release_index}
  arm64-apple-darwin-release CMAKE_TOOLCHAIN_FILE
  "${_cai_source_dir_literal}/cmake/toolchains/arm64-apple-darwin.cmake")
_cai_reject_cache_key(${_cai_darwin_release_index}
  arm64-apple-darwin-release CPKT_TARGET_ID)

_cai_find_configure_preset(debug-cai _cai_cli_native_index)
_cai_require_inherits(${_cai_cli_native_index} debug-cai debug)
_cai_require_cache_value(${_cai_cli_native_index} debug-cai CAI_BUILD_CLI ON)
string(JSON _cai_cli_binary_dir GET "${_cai_presets_text}" configurePresets ${_cai_cli_native_index} binaryDir)
if(NOT _cai_cli_binary_dir STREQUAL "${_cai_source_dir_literal}/build/debug")
  message(FATAL_ERROR "Native library and CLI feedback must share their Debug producer")
endif()
_cai_reject_cache_key(${_cai_cli_native_index} debug-cai CAI_BUILD_SHARED)
_cai_find_configure_preset(libcai _cai_library_index)
_cai_require_cache_value(${_cai_library_index} libcai CAI_BUILD_CLI OFF)
_cai_find_configure_preset(cai _cai_cli_release_index)
_cai_require_inherits(${_cai_cli_release_index} cai x86_64-linux-musl-release)
_cai_require_cache_value(${_cai_cli_release_index} cai CAI_BUILD_TESTS ON)
foreach(_cai_app_target IN ITEMS x86_64-linux-gnu x86_64-linux-musl
    aarch64-linux-gnu aarch64-linux-musl)
  _cai_find_configure_preset(${_cai_app_target}-release _cai_app_index)
  _cai_require_cache_value(${_cai_app_index} ${_cai_app_target}-release CAI_BUILD_APP ON)
endforeach()
_cai_find_configure_preset(base _cai_base_index)
_cai_require_cache_value(${_cai_base_index} base CAI_BUILD_STATIC ON)
_cai_require_cache_value(${_cai_base_index} base CAI_BUILD_SHARED ON)
# Native application regressions must be selected alongside CLI regressions.
string(JSON _cai_test_count LENGTH "${_cai_presets_text}" testPresets)
math(EXPR _cai_test_last "${_cai_test_count} - 1")
foreach(_cai_test_index RANGE 0 ${_cai_test_last})
  string(JSON _cai_test_name GET "${_cai_presets_text}" testPresets ${_cai_test_index} name)
  if(_cai_test_name STREQUAL "debug-cai")
    string(JSON _cai_test_filter GET "${_cai_presets_text}"
      testPresets ${_cai_test_index} filter include name)
    foreach(_cai_test_case IN ITEMS cai_cli_help cai_app cai_app_install cai_app_lua)
      if(NOT _cai_test_case MATCHES "${_cai_test_filter}")
        message(FATAL_ERROR "debug-cai must select ${_cai_test_case}")
      endif()
    endforeach()
  endif()
endforeach()
foreach(_cai_cross IN ITEMS x86_64-linux-musl-release aarch64-linux-gnu-release
    aarch64-linux-musl-release armhf-linux-gnu-release armhf-linux-musl-release
    arm64-apple-darwin-release)
  _cai_find_configure_preset(${_cai_cross} _cai_cross_index)
  _cai_require_cache_value(${_cai_cross_index} ${_cai_cross} CAI_BUILD_TESTS OFF)
endforeach()

_cai_require_cache_value(${_cai_fuzz_index} fuzz CAI_BUILD_TESTS OFF)
_cai_require_cache_value(${_cai_fuzz_index} fuzz CAI_BUILD_EXAMPLES OFF)
_cai_require_cache_value(${_cai_fuzz_index} fuzz CAI_BUILD_SHARED OFF)
