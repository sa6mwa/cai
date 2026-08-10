if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  if(NOT DEFINED CPKT_TARGET_ID OR CPKT_TARGET_ID STREQUAL "")
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _cpkt_host_processor)
    if(_cpkt_host_processor STREQUAL "x86_64" OR
       _cpkt_host_processor STREQUAL "amd64")
      set(CPKT_TARGET_ID "x86_64-linux-gnu" CACHE STRING
          "pkt.systems lifecycle target id" FORCE)
    else()
      message(FATAL_ERROR
        "native lifecycle Linux builds require an x86_64 host because the "
        "pinned Bootlin compiler collections are x86-hosted; got "
        "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
  endif()
  include("${CMAKE_CURRENT_LIST_DIR}/linux-bootlin.cmake")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  # Developer-only host-native Darwin builds. Release Darwin artifacts use the
  # explicit osxcross release preset and toolchain.
else()
  message(FATAL_ERROR
    "unsupported host system for native lifecycle toolchain: "
    "${CMAKE_HOST_SYSTEM_NAME}")
endif()
