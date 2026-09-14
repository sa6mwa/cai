function(cai_install_rpath_token target_id out_var)
  if(target_id MATCHES "darwin")
    set(${out_var} "@loader_path" PARENT_SCOPE)
  else()
    set(${out_var} "$ORIGIN" PARENT_SCOPE)
  endif()
endfunction()

function(cai_configure_local_runtime target)
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
     NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
     NOT CAI_TARGET_ID STREQUAL "x86_64-linux-gnu")
    return()
  endif()
  if(NOT DEFINED CMAKE_SYSROOT OR CMAKE_SYSROOT STREQUAL "" OR
     NOT DEFINED CPKT_BOOTLIN_ROOT OR CPKT_BOOTLIN_ROOT STREQUAL "")
    message(FATAL_ERROR
      "${target} requires the selected Bootlin sysroot and collection root")
  endif()
  file(GLOB _cai_loader_candidates "${CMAKE_SYSROOT}/lib/ld-linux*.so*")
  list(LENGTH _cai_loader_candidates _cai_loader_count)
  if(NOT _cai_loader_count EQUAL 1)
    message(FATAL_ERROR
      "${target} requires exactly one Bootlin ELF loader under ${CMAKE_SYSROOT}/lib")
  endif()
  list(GET _cai_loader_candidates 0 _cai_loader)
  set(_cai_runtime_dirs
    "${CMAKE_SYSROOT}/lib"
    "${CMAKE_SYSROOT}/usr/lib"
    "${CPKT_BOOTLIN_ROOT}/lib"
    "${CAI_C_PKT_SYSTEMS_PREFIX}/lib"
    "${CAI_LONEJSON_PREFIX}/lib")
  if(CAI_PSLOG_PREFIX)
    list(APPEND _cai_runtime_dirs "${CAI_PSLOG_PREFIX}/lib")
  endif()
  list(JOIN _cai_runtime_dirs ":" _cai_runtime_path)
  target_link_options(${target} PRIVATE
    "-Wl,--dynamic-linker,${_cai_loader}"
    "-Wl,--disable-new-dtags"
    "-Wl,-rpath,${_cai_runtime_path}")
endfunction()
