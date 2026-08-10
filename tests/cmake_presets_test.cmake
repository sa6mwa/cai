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

file(READ "${CAI_SOURCE_DIR}/CMakeLists.txt" _cai_cmake_lists_text)
if(_cai_cmake_lists_text MATCHES "option\\(CAI_USE_LOCKDC_SDK" OR
   _cai_cmake_lists_text MATCHES
   "CAI_DEPENDENCY_MODE STREQUAL \"(lockdc|pkt)\"" OR
   _cai_cmake_lists_text MATCHES "CAI_USE_LOCKDC_SDK")
  message(FATAL_ERROR
    "CMake must not retain deprecated lockdc or pkt dependency compatibility")
endif()
if(NOT _cai_cmake_lists_text MATCHES
   "set\\(CPKT_DEPENDENCY_CACHE \"\\$\\{_cai_dependency_cache_default\\}\" CACHE PATH")
  message(FATAL_ERROR
    "CMake must expose CPKT_DEPENDENCY_CACHE as the lifecycle cache variable")
endif()
if(NOT _cai_cmake_lists_text MATCHES
   "\"Shared checksum-pinned external archive cache\\.\" FORCE")
  message(FATAL_ERROR
    "CMake must force the resolved dependency cache value into CPKT_DEPENDENCY_CACHE")
endif()
if(NOT _cai_cmake_lists_text MATCHES
   "_cai_previous_dependency_cache_entries" OR
   NOT _cai_cmake_lists_text MATCHES
   "_cai_cai_dependency_cache_changed" OR
   NOT _cai_cmake_lists_text MATCHES
   "set\\(_cai_dependency_cache_default \"\\$\\{CAI_DEPENDENCY_CACHE\\}\"\\)")
  message(FATAL_ERROR
    "CMake must accept CAI_DEPENDENCY_CACHE as a legacy input alias on fresh and cached configures")
endif()
if(NOT _cai_cmake_lists_text MATCHES
   "set\\(CAI_DEPENDENCY_CACHE \"\\$\\{CPKT_DEPENDENCY_CACHE\\}\" CACHE PATH")
  message(FATAL_ERROR
    "CMake must keep CAI_DEPENDENCY_CACHE as a compatibility alias")
endif()
if(NOT _cai_cmake_lists_text MATCHES
   "CPKT_DEPENDENCY_CACHE}/archives/sha256/\\$\\{sha256\\}/")
  message(FATAL_ERROR
    "CMake dependency archives must use the lifecycle checksum-keyed shared cache layout")
endif()
if(NOT _cai_cmake_lists_text MATCHES
   "CPKT_DEPENDENCY_CACHE}/locks/\\$\\{sha256\\}\\.lock")
  message(FATAL_ERROR
    "CMake dependency archive locks must be keyed by checksum under locks/")
endif()
if(_cai_cmake_lists_text MATCHES "CPKT_DEPENDENCY_CACHE}/\\.locks" OR
   _cai_cmake_lists_text MATCHES
   "CPKT_DEPENDENCY_CACHE}/\\$\\{[^}]+_NAME\\}\\.tar\\.gz")
  message(FATAL_ERROR
    "CMake must not use the legacy flat dependency cache or .locks layout")
endif()
if(_cai_cmake_lists_text MATCHES "CMAKE_C_STANDARD" OR
   _cai_cmake_lists_text MATCHES "C_STANDARD[ \t\r\n]+90" OR
   _cai_cmake_lists_text MATCHES "C_STANDARD_REQUIRED" OR
   _cai_cmake_lists_text MATCHES "C_EXTENSIONS")
  message(FATAL_ERROR
    "project-owned C89 targets must use explicit compiler flags, not CMake C standard properties")
endif()
if(NOT _cai_cmake_lists_text MATCHES "-std=c89")
  message(FATAL_ERROR
    "project-owned C89 targets must retain the explicit -std=c89 compiler flag")
endif()

file(READ "${CAI_SOURCE_DIR}/cmake/CaiVersion.cmake" _cai_version_text)
if(NOT _cai_version_text MATCHES "scripts/release_version\\.sh")
  message(FATAL_ERROR
    "CMake version detection must use the lifecycle release_version.sh script")
endif()

file(READ "${CAI_SOURCE_DIR}/cmake/CaiLonejsonAbi.cmake"
     _cai_lonejson_abi_text)
if(NOT _cai_lonejson_abi_text MATCHES "CMAKE_READELF" OR
   NOT _cai_lonejson_abi_text MATCHES "CMAKE_OBJDUMP" OR
   NOT _cai_lonejson_abi_text MATCHES "CPKT_OTOOL")
  message(FATAL_ERROR
    "lonejson ABI validation must prefer configured target inspection tools")
endif()

file(READ "${CAI_SOURCE_DIR}/cmake/package_archive.cmake"
     _cai_package_archive_text)
if(_cai_package_archive_text MATCHES
   "find_program\\(CAI_STRIP_BIN NAMES strip\\)")
  message(FATAL_ERROR
    "package archives must not fall back to an ambient strip from PATH")
endif()

file(READ "${CAI_SOURCE_DIR}/cmake/toolchains/linux-bootlin.cmake"
     _cai_linux_bootlin_toolchain_text)
file(READ "${CAI_SOURCE_DIR}/cmake/toolchains/native-lifecycle.cmake"
     _cai_native_lifecycle_toolchain_text)
if(NOT _cai_native_lifecycle_toolchain_text MATCHES
   "CMAKE_HOST_SYSTEM_NAME STREQUAL \"Linux\"" OR
   NOT _cai_native_lifecycle_toolchain_text MATCHES
   "include\\(\"\\$\\{CMAKE_CURRENT_LIST_DIR\\}/linux-bootlin\\.cmake\"\\)" OR
   NOT _cai_native_lifecycle_toolchain_text MATCHES
   "CMAKE_HOST_SYSTEM_NAME STREQUAL \"Darwin\"" OR
   NOT _cai_native_lifecycle_toolchain_text MATCHES
   "pinned Bootlin compiler collections are x86-hosted")
  message(FATAL_ERROR
    "native lifecycle toolchain must route x86_64 Linux through Bootlin, reject unsupported Linux hosts, and preserve Darwin host-native developer builds")
endif()
if(NOT _cai_linux_bootlin_toolchain_text MATCHES
   "set\\(CMAKE_FIND_ROOT_PATH \"\\$\\{_cpkt_sysroot\\}\" \"\\$\\{_cpkt_root\\}\" CACHE STRING \"\" FORCE\\)")
  message(FATAL_ERROR
    "Linux Bootlin toolchain must search both the selected sysroot and Bootlin root")
endif()
if(NOT _cai_linux_bootlin_toolchain_text MATCHES
   "CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING \"\" FORCE" OR
   NOT _cai_linux_bootlin_toolchain_text MATCHES
   "CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING \"\" FORCE")
  message(FATAL_ERROR
    "Linux Bootlin toolchain find-root modes must be pinned in the CMake cache")
endif()
if(NOT _cai_linux_bootlin_toolchain_text MATCHES "-print-prog-name=ld" OR
   NOT _cai_linux_bootlin_toolchain_text MATCHES "-print-file-name=libc\\.so" OR
   NOT _cai_linux_bootlin_toolchain_text MATCHES
   "compiler selects linker outside Bootlin root" OR
   NOT _cai_linux_bootlin_toolchain_text MATCHES
   "compiler selects libc outside Bootlin sysroot")
  message(FATAL_ERROR
    "Linux Bootlin toolchain must assert selected linker and libc stay inside the pinned collection")
endif()
foreach(_cai_tool IN ITEMS
    CMAKE_LINKER CMAKE_AR CMAKE_RANLIB CMAKE_STRIP CMAKE_NM
    CMAKE_OBJCOPY CMAKE_OBJDUMP CMAKE_ADDR2LINE CMAKE_GDB CMAKE_READELF)
  if(NOT _cai_linux_bootlin_toolchain_text MATCHES
     "set\\(${_cai_tool} \"\\$\\{_cpkt_[A-Za-z0-9_]+\\}\" CACHE FILEPATH \"\" FORCE\\)")
    message(FATAL_ERROR
      "Linux Bootlin toolchain must force ${_cai_tool} from the pinned collection")
  endif()
endforeach()

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

foreach(_cai_native_preset IN ITEMS release asan valgrind)
  _cai_find_configure_preset(${_cai_native_preset} _cai_native_index)
  _cai_require_inherits(${_cai_native_index} ${_cai_native_preset} bootlin-native)
endforeach()
foreach(_cai_debug_child IN ITEMS debug-lua coverage integration)
  _cai_find_configure_preset(${_cai_debug_child} _cai_debug_child_index)
  _cai_require_inherits(${_cai_debug_child_index} ${_cai_debug_child} debug)
endforeach()

_cai_find_configure_preset(valgrind _cai_valgrind_index)
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
