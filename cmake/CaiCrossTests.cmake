# Cross runtime proof stays separate from the native regression inventory.
if(NOT CAI_TARGET_ID MATCHES "^(aarch64|armhf)-linux-(gnu|musl)$")
  return()
endif()
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Linux ARM runtime checks require a Linux QEMU host")
endif()
if(CAI_TARGET_ID MATCHES "^aarch64-")
  set(_cai_qemu_name qemu-aarch64)
else()
  set(_cai_qemu_name qemu-arm)
endif()
find_program(CAI_QEMU_EXECUTABLE NAMES "${_cai_qemu_name}" REQUIRED)
if(NOT EXISTS "${CAI_QEMU_EXECUTABLE}" OR NOT IS_DIRECTORY "${CMAKE_SYSROOT}/lib")
  message(FATAL_ERROR "ARM runtime checks require ${_cai_qemu_name} and the selected toolchain sysroot")
endif()

# Set the guest loader environment, never the host QEMU loader environment.
set(_cai_guest_dirs "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SYSROOT}/lib"
  "${CMAKE_SYSROOT}/usr/lib")
foreach(_prefix CAI_CPKT_PREFIX CAI_LONEJSON_PREFIX CAI_PSLOG_PREFIX
    _cai_softline_prefix _cai_libmdf_prefix _cai_lockdc_prefix _cai_liblql_prefix)
  if(DEFINED ${_prefix} AND NOT "${${_prefix}}" STREQUAL "")
    list(APPEND _cai_guest_dirs "${${_prefix}}/lib")
  endif()
endforeach()
list(JOIN _cai_guest_dirs ":" _cai_guest_path)
set(_cai_qemu_command "${CAI_QEMU_EXECUTABLE}" -L "${CMAKE_SYSROOT}"
  -E "LD_LIBRARY_PATH=${_cai_guest_path}")
enable_testing()
foreach(_variant static shared)
  if(TARGET cai_${_variant})
    add_executable(cai_cross_${_variant}_smoke tests/cross_runtime_smoke.c)
    target_link_libraries(cai_cross_${_variant}_smoke PRIVATE cai_${_variant})
    if(TARGET cai_app_${_variant})
      target_link_libraries(cai_cross_${_variant}_smoke PRIVATE cai_app_${_variant})
      target_compile_definitions(cai_cross_${_variant}_smoke PRIVATE CAI_SMOKE_WITH_APP)
    endif()
    cai_configure_c_target(cai_cross_${_variant}_smoke)
    target_compile_options(cai_cross_${_variant}_smoke PRIVATE -UNDEBUG)
    if(_variant STREQUAL "static" AND CAI_TARGET_ID MATCHES "-musl$")
      target_link_options(cai_cross_${_variant}_smoke PRIVATE -static)
    endif()
    set_property(TARGET cai_cross_${_variant}_smoke PROPERTY
      CROSSCOMPILING_EMULATOR "${_cai_qemu_command}")
    add_test(NAME cai_cross_${_variant}_smoke COMMAND cai_cross_${_variant}_smoke)
    set_tests_properties(cai_cross_${_variant}_smoke PROPERTIES
      LABELS "cross;smoke;offline" TIMEOUT 10)
  endif()
endforeach()
if(TARGET cai_cli)
  add_test(NAME cai_cross_cli_smoke COMMAND "${CMAKE_COMMAND}"
    "-DCAI_RUNNER=${_cai_qemu_command}" "-DCAI_BINARY=$<TARGET_FILE:cai_cli>"
    "-DCAI_EXPECTED_VERSION=${CAI_PACKAGE_VERSION}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cross_cli_smoke.cmake")
  set_tests_properties(cai_cross_cli_smoke PROPERTIES
    LABELS "cross;smoke;offline" TIMEOUT 5)
endif()
