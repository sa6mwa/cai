set(scratch "${CAI_BINARY_DIR}/cpkt-target-mismatch")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/package")
file(READ "${CAI_BINARY_DIR}/cai-config.cmake" config)
string(REPLACE "set(cai_CPKT_TARGET \"${CAI_TARGET_ID}\")"
  "set(cai_CPKT_TARGET \"wrong-target\")" config "${config}")
file(WRITE "${scratch}/package/cai-config.cmake" "${config}")
file(COPY "${CAI_SOURCE_DIR}/cmake/CaiLonejsonAbi.cmake"
  DESTINATION "${scratch}/package")
file(WRITE "${scratch}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.21)\nproject(wrong_target C)\nlist(APPEND CMAKE_FIND_ROOT_PATH \"${CAI_CPKT_PREFIX}\")\nfind_package(cai CONFIG REQUIRED)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${scratch}" -B "${scratch}/build"
  "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
  "-DCMAKE_TOOLCHAIN_FILE=${CAI_TOOLCHAIN_FILE}"
  "-DCPKT_TARGET_ID=${CAI_TARGET_ID}"
  "-Dcai_DIR=${scratch}/package" "-DCMAKE_PREFIX_PATH=${CAI_CPKT_PREFIX}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "cai requires cpkt target wrong-target, found ${CAI_TARGET_ID}")
  message(FATAL_ERROR "Wrong-target SDK was not rejected clearly:\n${output}\n${error}")
endif()
