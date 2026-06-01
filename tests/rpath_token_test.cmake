if(NOT DEFINED CAI_SOURCE_DIR)
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()

include("${CAI_SOURCE_DIR}/cmake/cai-rpath.cmake")

cai_install_rpath_token("x86_64-linux-gnu" linux_token)
if(NOT linux_token STREQUAL "$ORIGIN")
  message(FATAL_ERROR "linux rpath token mismatch: ${linux_token}")
endif()

cai_install_rpath_token("x86_64-linux-musl" musl_token)
if(NOT musl_token STREQUAL "$ORIGIN")
  message(FATAL_ERROR "linux musl rpath token mismatch: ${musl_token}")
endif()

cai_install_rpath_token("arm64-apple-darwin" darwin_token)
if(NOT darwin_token STREQUAL "@loader_path")
  message(FATAL_ERROR "darwin rpath token mismatch: ${darwin_token}")
endif()
