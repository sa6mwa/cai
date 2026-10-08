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
string(TOUPPER "${CMAKE_BUILD_TYPE}" yaml_configuration)
set(yaml_configuration_flags "${CMAKE_C_FLAGS_${yaml_configuration}}")
set(yaml_flags "${CMAKE_C_FLAGS}")
set(yaml_release_flags "${CMAKE_C_FLAGS_RELEASE}")
if(CAI_ENABLE_COVERAGE)
  set(yaml_profile coverage)
  string(APPEND yaml_flags " ${yaml_configuration_flags} --coverage")
  set(yaml_release_flags "")
elseif(CAI_BUILD_FUZZERS)
  set(yaml_profile fuzz)
  string(APPEND yaml_flags " ${yaml_configuration_flags}")
  set(yaml_release_flags "")
elseif("${CMAKE_C_FLAGS} ${yaml_configuration_flags}" MATCHES "fsanitize")
  set(yaml_profile sanitizer)
  string(APPEND yaml_flags " ${yaml_configuration_flags}")
  set(yaml_release_flags "")
endif()
set(CAI_LIBYAML_PREFIX "${CAI_DEPS_DIR}/libyaml-${CAI_LIBYAML_VERSION}-${CAI_TARGET_ID}-${yaml_profile}")
set(CAI_LIBYAML_BUILD_DIR "${CAI_LIBYAML_PREFIX}/build")
function(cai_configure_yaml)
  file(MAKE_DIRECTORY "${CAI_LIBYAML_PREFIX}")
  file(LOCK "${CAI_LIBYAML_PREFIX}/build.lock" GUARD FUNCTION TIMEOUT 600)
  set(arguments -G Ninja "-DCAI_YAML_SOURCE=${CAI_LIBYAML_SOURCE}"
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_C_FLAGS=${yaml_flags}"
    "-DCMAKE_C_FLAGS_RELEASE=${yaml_release_flags}"
    "-DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
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
    -B "${CAI_LIBYAML_BUILD_DIR}" ${arguments} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot configure private libyaml producer")
  endif()
endfunction()
cai_configure_yaml()
# The parent owns the producer dependency; the child Ninja graph owns source,
# header and wrapper invalidation. One nested job stays within the parent limit.
# No compilation/installation occurs during configure.
add_custom_target(cai_yaml_build
  COMMAND "${CMAKE_COMMAND}" --build "${CAI_LIBYAML_BUILD_DIR}" --parallel 1
  BYPRODUCTS
    "${CAI_LIBYAML_BUILD_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}yaml${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${CAI_LIBYAML_BUILD_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}yaml${CMAKE_SHARED_LIBRARY_SUFFIX}"
  COMMENT "Build reusable private libyaml producer"
  VERBATIM)
add_library(cai_yaml_static STATIC IMPORTED GLOBAL)
set_target_properties(cai_yaml_static PROPERTIES
  IMPORTED_LOCATION "${CAI_LIBYAML_BUILD_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}yaml${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "${CAI_LIBYAML_SOURCE}/include")
add_dependencies(cai_yaml_static cai_yaml_build)
