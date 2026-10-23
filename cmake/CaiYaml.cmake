# Consume only the YAML component from the separately installed cpktmisc SDK.
set(CAI_CPKTMISC_VERSION "0.2.0")
set(CAI_LIBYAML_VERSION "0.2.5")
function(cai_cpktmisc_sha256 target output)
  if(target STREQUAL "x86_64-linux-gnu")
    set(sha "abac4dff1f51d98c8403f078279a0fc290b1c1e37539c879e3365b9500786d50")
  elseif(target STREQUAL "x86_64-linux-musl")
    set(sha "89f4d9e309f34a6cd903dedecfa57c94a1f5619d7765603ff08d7826879c580e")
  elseif(target STREQUAL "aarch64-linux-gnu")
    set(sha "179fcde812eedfb566797a09edadda9bf32e99f618aa2b97f3cfaf75fe065733")
  elseif(target STREQUAL "aarch64-linux-musl")
    set(sha "3e031026e1913fd0ec8c544445fbb6c4c9e71270750dd6e10d6bff0afb8be2cb")
  elseif(target STREQUAL "armhf-linux-gnu")
    set(sha "7efa0e4ec6b51edbf7dc36f0f7ab3de3dae846c1ce1faee1b844da393bea3693")
  elseif(target STREQUAL "armhf-linux-musl")
    set(sha "d4921e6bf8b605b87b976470db84370820bc431245335d3fd9268f3ad7abe54b")
  elseif(target STREQUAL "arm64-apple-darwin")
    set(sha "f4c4790a63af3cfb0315bb7c453ab6bb2b3841bbdc98b9db3a5900c6cd20bd19")
  else()
    message(FATAL_ERROR "No cpktmisc ${CAI_CPKTMISC_VERSION} SDK pin for ${target}")
  endif()
  set(${output} "${sha}" PARENT_SCOPE)
endfunction()

set(CAI_CPKTMISC_URL_FOR_PACKAGE "host")
set(CAI_CPKTMISC_SHA256_FOR_PACKAGE "host")
set(CAI_CPKTMISC_ARCHIVE_NAME_FOR_PACKAGE "host")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  cai_cpktmisc_sha256("${CAI_TARGET_ID}" CAI_CPKTMISC_SHA256_FOR_PACKAGE)
  set(CAI_CPKTMISC_ARCHIVE_NAME_FOR_PACKAGE
    "cpktmisc-${CAI_CPKTMISC_VERSION}-${CAI_TARGET_ID}.tar.gz")
  set(CAI_CPKTMISC_URL_FOR_PACKAGE
    "https://github.com/c89systems/cpktmisc/releases/download/v${CAI_CPKTMISC_VERSION}/${CAI_CPKTMISC_ARCHIVE_NAME_FOR_PACKAGE}")
  set(CAI_CPKTMISC_PREFIX "${CAI_DEPS_DIR}/cpktmisc-${CAI_CPKTMISC_VERSION}-${CAI_TARGET_ID}")
  cai_dependency_archive_path("${CAI_CPKTMISC_ARCHIVE_NAME_FOR_PACKAGE}"
    "${CAI_CPKTMISC_SHA256_FOR_PACKAGE}" misc_archive)
  cai_download_tarball("cpktmisc ${CAI_CPKTMISC_VERSION} ${CAI_TARGET_ID}"
    "${CAI_CPKTMISC_URL_FOR_PACKAGE}" "${misc_archive}" "${CAI_CPKTMISC_PREFIX}"
    "${CAI_CPKTMISC_SHA256_FOR_PACKAGE}"
    "${CAI_CPKTMISC_PREFIX}/lib/cmake/CpktYaml/CpktYamlConfig.cmake")
  list(PREPEND CMAKE_PREFIX_PATH "${CAI_CPKTMISC_PREFIX}")
  if(CMAKE_CROSSCOMPILING)
    list(APPEND CMAKE_FIND_ROOT_PATH "${CAI_CPKTMISC_PREFIX}")
  endif()
  set(CpktMisc_DIR "${CAI_CPKTMISC_PREFIX}/lib/cmake/CpktMisc"
    CACHE PATH "cpktmisc package directory" FORCE)
  set(CpktYaml_DIR "${CAI_CPKTMISC_PREFIX}/lib/cmake/CpktYaml"
    CACHE PATH "YAML package directory" FORCE)
endif()

# CpktYaml validates cpktmisc's manifest and its compatible, separate core SDK.
find_package(CpktYaml ${CAI_LIBYAML_VERSION} EXACT CONFIG REQUIRED)
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt" AND
    NOT CpktMisc_VERSION VERSION_EQUAL CAI_CPKTMISC_VERSION)
  message(FATAL_ERROR "Expected cpktmisc ${CAI_CPKTMISC_VERSION}, found ${CpktMisc_VERSION}")
endif()
get_target_property(_cai_yaml_archive CpktYaml::Yaml IMPORTED_LOCATION)
get_filename_component(_cai_yaml_libdir "${_cai_yaml_archive}" DIRECTORY)
get_filename_component(CAI_CPKTMISC_PREFIX "${_cai_yaml_libdir}" DIRECTORY)
file(READ "${CAI_CPKTMISC_PREFIX}/share/cpkt/packages/misc.json" _cai_misc_manifest)
string(JSON CAI_CPKTMISC_PACKAGE_ID_FOR_PACKAGE GET "${_cai_misc_manifest}" package_id)
set(CAI_CPKTMISC_VERSION "${CpktMisc_VERSION}")
set(CAI_LIBYAML_LICENSE "${CAI_CPKTMISC_PREFIX}/share/doc/cpkt/misc/third_party/libyaml/LICENSE")
if(NOT EXISTS "${CAI_LIBYAML_LICENSE}")
  message(FATAL_ERROR "Selected cpktmisc SDK is missing the libyaml license: ${CAI_LIBYAML_LICENSE}")
endif()
