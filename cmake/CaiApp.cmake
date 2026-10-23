add_library(cai_app_objects OBJECT cli/app.c cli/app_run.c cli/kanban.c)
target_link_libraries(cai_app_objects PRIVATE cai_cli_support)
cai_configure_c_target(cai_app_objects)
add_library(cai_app_static STATIC $<TARGET_OBJECTS:cai_app_objects>
  $<TARGET_OBJECTS:cai_cli_support>)
add_library(cai::app_static ALIAS cai_app_static)
set_target_properties(cai_app_static PROPERTIES OUTPUT_NAME cai_app EXPORT_NAME app_static)
target_include_directories(cai_app_static PUBLIC
  $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(cai_app_static PUBLIC cai_static softline::softline_static
  libmdf::mdf_static lockdc::static)
if(CAI_BUILD_SHARED)
  find_package(lockdc CONFIG REQUIRED COMPONENTS shared)
  add_library(cai_app_shared SHARED $<TARGET_OBJECTS:cai_app_objects>
    $<TARGET_OBJECTS:cai_cli_support>)
  add_library(cai::app_shared ALIAS cai_app_shared)
  set_target_properties(cai_app_shared PROPERTIES OUTPUT_NAME cai_app EXPORT_NAME app_shared
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  target_include_directories(cai_app_shared PUBLIC
    $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(cai_app_shared PUBLIC cai_shared pslog::pslog_shared PRIVATE
    softline::softline libmdf::mdf_shared lockdc::shared)
  target_link_options(cai_app_shared PRIVATE -Wl,--no-undefined)
  cai_apply_export_policy(cai_app_shared "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app.exports")
  cai_configure_c_target(cai_app_shared)
endif()
if(CAI_BUILD_TESTS)
  add_executable(cai_kanban_test tests/kanban_test.c)
  target_sources(cai_kanban_test PRIVATE tests/cli_storage_fixture.c)
  target_link_options(cai_kanban_test PRIVATE "-Wl,--wrap=lc_client_open")
  target_link_libraries(cai_kanban_test PRIVATE cai_app_static)
  target_include_directories(cai_kanban_test PRIVATE "${CAI_PSLOG_INCLUDE_DIR}")
  cai_configure_cli_target(cai_kanban_test)
  target_compile_options(cai_kanban_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_kanban COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kanban_test.sh"
    "$<TARGET_FILE:cai_kanban_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app_kanban PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)
  add_executable(cai_app_test tests/app_test.c)
  if(TARGET cai_app_shared AND NOT CAI_TARGET_ID MATCHES "-linux-musl$")
    target_link_libraries(cai_app_test PRIVATE cai_app_shared)
  else()
    target_link_libraries(cai_app_test PRIVATE cai_app_static)
  endif()
  cai_configure_cli_target(cai_app_test)
  target_include_directories(cai_app_test PRIVATE "${CAI_PSLOG_INCLUDE_DIR}")
  target_compile_options(cai_app_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_test.sh"
    "$<TARGET_FILE:cai_app_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)
  add_executable(cai_app_host tests/app_host.c)
  target_link_libraries(cai_app_host PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_host)
  target_compile_options(cai_app_host PRIVATE -UNDEBUG)
  if(CAI_BUILD_CLI)
    add_test(NAME cai_app_embedding COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_embedding_test.sh" "$<TARGET_FILE:cai_app_host>")
    set_tests_properties(cai_app_embedding PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
  endif()
  if(TARGET cai_app_shared)
    add_test(NAME cai_app_exports COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh"
      "${CMAKE_NM}" "$<TARGET_FILE:cai_app_shared>"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app.exports" linux)
    set_tests_properties(cai_app_exports PROPERTIES LABELS "cli;local;offline" TIMEOUT 5)
  endif()
  if(CAI_INSTALL)
    set(_app_lua_runner "")
    if(CAI_BUILD_LUA)
      set(_app_lua_runner "$<TARGET_FILE:cai_lua_runner>")
    endif()
    set(_app_prefixes "${CAI_CPKT_PREFIX};${CAI_LONEJSON_PREFIX};${_cai_softline_prefix};${_cai_libmdf_prefix};${_cai_lockdc_prefix};${_cai_liblql_prefix};${CAI_DEPS_DIR}/libpslog-${CAI_PSLOG_VERSION}-${CAI_TARGET_ID}")
    add_test(NAME cai_app_install COMMAND "${CMAKE_COMMAND}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCAI_DEPENDENCY_PREFIXES=${_app_prefixes}"
      "-DCAI_LUA_RUNNER=${_app_lua_runner}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_install_test.cmake")
    set_tests_properties(cai_app_install PROPERTIES LABELS "cli;local;offline;packaging" TIMEOUT 30)
  endif()
endif()
if(CAI_INSTALL)
  install(TARGETS cai_app_static EXPORT caiAppTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app)
  if(TARGET cai_app_shared)
    install(TARGETS cai_app_shared EXPORT caiAppTargets
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app)
  endif()
  install(FILES "${CAI_PUBLIC_INCLUDE_DIR}/cai/app.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cai" COMPONENT cai-app)
  install(EXPORT caiAppTargets NAMESPACE cai:: FILE cai-app-targets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app" COMPONENT cai-app)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config-version.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config-version.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-app.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-app.pc" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app-dependencies.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app-dependencies.json" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/app-dependencies.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cai_app" RENAME dependencies.json COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app" COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai-app.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/embedding.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/agent-messaging.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/kanban.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai_app" COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cli/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/app.lua"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai_app/examples" COMPONENT cai-app)
endif()
