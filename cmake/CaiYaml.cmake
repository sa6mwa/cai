# Temporary private source producer until libyaml joins c.pkt.systems core.
set(CAI_LIBYAML_VERSION "0.2.5")
set(CAI_LIBYAML_URL "https://github.com/yaml/libyaml/archive/refs/tags/0.2.5.tar.gz")
set(CAI_LIBYAML_SHA256 "fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e")
cai_dependency_archive_path("libyaml-0.2.5.tar.gz" "${CAI_LIBYAML_SHA256}" yaml_archive)
set(CAI_LIBYAML_SOURCE "${CAI_DEPS_DIR}/libyaml-0.2.5")
cai_download_tarball("libyaml ${CAI_LIBYAML_VERSION} source" "${CAI_LIBYAML_URL}"
  "${yaml_archive}" "${CAI_LIBYAML_SOURCE}" "${CAI_LIBYAML_SHA256}"
  "${CAI_LIBYAML_SOURCE}/src/parser.c")
set(yaml_profile normal)
if(CAI_ENABLE_COVERAGE)
  set(yaml_profile coverage)
elseif(CAI_BUILD_FUZZERS)
  set(yaml_profile fuzz)
elseif(CMAKE_C_FLAGS MATCHES "fsanitize")
  set(yaml_profile sanitizer)
endif()
set(CAI_LIBYAML_PREFIX "${CAI_DEPS_DIR}/libyaml-${CAI_LIBYAML_VERSION}-${CAI_TARGET_ID}-${yaml_profile}")
function(cai_prepare_yaml)
  file(MAKE_DIRECTORY "${CAI_LIBYAML_PREFIX}")
  file(LOCK "${CAI_LIBYAML_PREFIX}/build.lock" GUARD FUNCTION TIMEOUT 600)
  set(arguments -G Ninja "-DCAI_YAML_SOURCE=${CAI_LIBYAML_SOURCE}"
    "-DCMAKE_INSTALL_PREFIX=${CAI_LIBYAML_PREFIX}" -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    "-DCMAKE_AR=${CMAKE_AR}" "-DCMAKE_RANLIB=${CMAKE_RANLIB}")
  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND arguments "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      "-DCPKT_TARGET_ID=${CPKT_TARGET_ID}")
  endif()
  if(CMAKE_SYSROOT)
    list(APPEND arguments "-DCMAKE_SYSROOT=${CMAKE_SYSROOT}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/libyaml"
    -B "${CAI_LIBYAML_PREFIX}/build" ${arguments} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot configure private libyaml producer")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${CAI_LIBYAML_PREFIX}/build"
    --parallel 4 RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot build private libyaml producer")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --install "${CAI_LIBYAML_PREFIX}/build"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot install private libyaml producer")
  endif()
endfunction()
cai_prepare_yaml()
add_library(cai_yaml_static STATIC IMPORTED GLOBAL)
set_target_properties(cai_yaml_static PROPERTIES
  IMPORTED_LOCATION "${CAI_LIBYAML_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}yaml${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "${CAI_LIBYAML_PREFIX}/include")
