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
